/****************************************************************************/
// Eclipse SUMO, Simulation of Urban MObility; see https://eclipse.dev/sumo
// Copyright (C) 2011-2025 German Aerospace Center (DLR) and others.
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License 2.0 which is available at
// https://www.eclipse.org/legal/epl-2.0/
// This Source Code may also be made available under the following Secondary
// Licenses when the conditions for such availability set forth in the Eclipse
// Public License 2.0 are satisfied: GNU General Public License, version 2
// or later which is available at
// https://www.gnu.org/licenses/old-licenses/gpl-2.0-standalone.html
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later
/****************************************************************************/
/// @file    NWWriter_OpenDrive.cpp
/// @author  Daniel Krajzewicz
/// @author  Jakob Erdmann
/// @date    Tue, 04.05.2011
///
// Exporter writing networks using the openDRIVE format
/****************************************************************************/
#include <config.h>

#include <ctime>
#include "NWWriter_OpenDrive.h"
#include <utils/iodevices/OutputDevice_String.h>
#include <utils/common/MsgHandler.h>
#include <netbuild/NBEdgeCont.h>
#include <netbuild/NBNode.h>
#include <netbuild/NBNodeCont.h>
#include <netbuild/NBNetBuilder.h>
#include <utils/options/OptionsCont.h>
#include <utils/iodevices/OutputDevice.h>
#include <utils/common/MsgHandler.h>
#include <utils/common/StdDefs.h>
#include <utils/common/StringUtils.h>
#include <utils/common/StringTokenizer.h>
#include <utils/geom/GeoConvHelper.h>
#include <regex>

#define INVALID_ID -1

//#define DEBUG_SMOOTH_GEOM
#define DEBUGCOND true

#define MIN_TURN_DIAMETER 2.0

#define ROAD_OBJECTS "roadObjects"

// ===========================================================================
// static members
// ===========================================================================
bool NWWriter_OpenDrive::lefthand(false);
bool NWWriter_OpenDrive::LHLL(false);
bool NWWriter_OpenDrive::LHRL(false);

static std::unordered_map<std::string, int> id_redirections;
static int id_increment(0);
static std::set<std::string> controlled_signal_ids;
// ===========================================================================
// method definitions
// ===========================================================================

NWWriter_OpenDrive::OpenDRIVERoad::OpenDRIVERoad(int idd) : id(idd) {
    road_device = std::make_shared<OutputDevice_String>();
    signals_device = std::make_shared<OutputDevice_String>();
}
NWWriter_OpenDrive::OpenDRIVERoad::OpenDRIVERoad() {
    road_device = std::make_shared<OutputDevice_String>();
    signals_device = std::make_shared<OutputDevice_String>();
}
// ---------------------------------------------------------------------------
// static methods
// ---------------------------------------------------------------------------
void
NWWriter_OpenDrive::writeNetwork(const OptionsCont& oc, NBNetBuilder& nb) {
    // check whether an opendrive-file shall be generated
    if (!oc.isSet("opendrive-output")) {
        return;
    }
    id_redirections.clear();
    const NBNodeCont& nc = nb.getNodeCont();
    const NBEdgeCont& ec = nb.getEdgeCont();
    const bool origNames = oc.getBool("output.original-names");
    lefthand = oc.getBool("lefthand");
    LHLL = lefthand && oc.getBool("opendrive-output.lefthand-left");
    LHRL = lefthand && !LHLL;
    const double straightThresh = DEG2RAD(oc.getFloat("opendrive-output.straight-threshold"));
    // some internal mapping containers
    int nodeID = 1;
    int edgeID = nc.size() * 10; // distinct from node ids
    int signalID = 0;
    int controllerID = 0;
    StringBijection<int> edgeMap;
    StringBijection<int> nodeMap;
    //
    OutputDevice& device = OutputDevice::getDevice(oc.getString("opendrive-output"));
    OutputDevice::createDeviceByOption("opendrive-output", "OpenDRIVE");
    time_t now = time(nullptr);
    std::string dstr(ctime(&now));
    const Boundary& b = GeoConvHelper::getFinal().getConvBoundary();
    // write header
    device.openTag("header");
    device.writeAttr("revMajor", "1");
    device.writeAttr("revMinor", "4");
    device.writeAttr("name", "");
    device.writeAttr("version", "1.00");
    device.writeAttr("date", dstr.substr(0, dstr.length() - 1));
    device.writeAttr("north", b.ymax());
    device.writeAttr("south", b.ymin());
    device.writeAttr("east", b.xmax());
    device.writeAttr("west", b.xmin());
    /* @note obsolete in 1.4
    device.writeAttr("maxRoad", ec.size());
    device.writeAttr("maxJunc", nc.size());
    device.writeAttr("maxPrg", 0);
    */
    // write optional geo reference
    const GeoConvHelper& gch = GeoConvHelper::getFinal();
    if (gch.usingGeoProjection()) {
        device.openTag("geoReference");
        device.writePreformattedTag(" <![CDATA[\n "
                                    + gch.getProjString()
                                    + "\n]]>\n");
        device.closeTag();
        if (gch.getOffsetBase() != Position(0, 0)) {
            device.openTag("offset");
            device.writeAttr("x", -gch.getOffsetBase().x());
            device.writeAttr("y", -gch.getOffsetBase().y());
            device.writeAttr("z", -gch.getOffsetBase().z());
            device.writeAttr("hdg", 0);
            device.closeTag();
        } else {
            WRITE_WARNING("Could not write OpenDRIVE geoReference. Only unshifted Coordinate systems are supported (center_map and use_offsets need to be set to False)");
        }
    }
    device.closeTag();

    SignalLanes signalLanes;

    mapmatchRoadObjects(nb.getShapeCont(), ec);

    PositionVector crosswalk_shape;
    std::map<std::string, std::vector<std::string>> crosswalksByEdge;
    for (auto it = nc.begin(); it != nc.end(); ++it) {
        NBNode* n = it->second;
        auto crosswalks = n->getCrossings();
        for (const auto& cw : n->getCrossings()) {
            // getting from crosswalk line to a full shape
            crosswalk_shape = cw->shape;
            PositionVector rightside = cw->shape;
            try {
                crosswalk_shape.move2side(cw->width / 2);
                rightside.move2side(cw->width / -2);
            } catch (InvalidArgument&) { }
            rightside = rightside.reverse();
            crosswalk_shape.append(rightside);
            auto crosswalkId = cw->id;
            nb.getShapeCont().addPolygon(crosswalkId, "crosswalk", RGBColor::DEFAULT_COLOR, 0,
                                         Shape::DEFAULT_ANGLE, Shape::DEFAULT_IMG_FILE,
                                         crosswalk_shape, false, true, 1, false, crosswalkId);
            SUMOPolygon* cwp = nb.getShapeCont().getPolygons().get(crosswalkId);
            cwp->setParameter("length", toString(cw->width));
            cwp->setParameter("width", toString(cw->shape.length2D()));
            cwp->setParameter("hdg", "0");
            crosswalksByEdge[cw->edges[0]->getID()].push_back(crosswalkId);
        }
    }
    device.closeTag();

    // write normal edges (road)
    for (std::map<std::string, NBEdge*>::const_iterator i = ec.begin(); i != ec.end(); ++i) {
        const NBEdge* e = (*i).second;
        const int fromNodeID = e->getIncomingEdges().size() > 0 ? getID(e->getFromNode()->getID(), nodeMap, nodeID) : INVALID_ID;
        const int toNodeID = e->getConnections().size() > 0 ? getID(e->getToNode()->getID(), nodeMap, nodeID) : INVALID_ID;
        writeNormalEdge(device, e,
                        getID(e->getID(), edgeMap, edgeID),
                        fromNodeID, toNodeID,
                        origNames, straightThresh,
                        nb.getShapeCont(),
                        signalLanes,
                        crosswalksByEdge[e->getID()]);
    }
    device.lf();


    // auxiliary structs to store traffic light position effects
    struct TLReference {
        Position position;
        double heading;
    };
    struct OpenDRIVETL {
        int id = -1;
        std::vector<TLReference> references;
    };
    // write junction-internal edges (road). In OpenDRIVE these are called 'paths' or 'connecting roads'
    OutputDevice_String junctionOSS(3);
    OutputDevice_String controllersOSS(3);
    for (std::map<std::string, NBNode*>::const_iterator i = nc.begin(); i != nc.end(); ++i) {
        OutputDevice_String junction_intermediateOSS(3);
        std::vector<OpenDRIVERoad> roads;
        int num_connections = 0;
        NBNode* n = (*i).second;
        int connectionID = 0; // unique within a junction
        const int nID = getID(n->getID(), nodeMap, nodeID);
        if (n->numNormalConnections() > 0) {
            junction_intermediateOSS << "    <junction name=\"" << n->getID() << "\" id=\"" << nID << "\">\n";
        }
        std::vector<NBEdge*> incoming = (*i).second->getIncomingEdges();
        if (lefthand) {
            std::reverse(incoming.begin(), incoming.end());
        }
        // store controller record to be written inside the junction record
        OutputDevice_String junctioncontrollerOSS(2);
        // list of traffic lights
        std::vector<OpenDRIVETL> traffic_lights;
        // for each road reaching the junction
        for (NBEdge* inEdge : incoming) {
            OpenDRIVETL traffic_light;
            traffic_light.id = signalID;
            int num_connecting_roads = 0;
            // signal reference to the traffic light (where it has effect)
            OutputDevice_String signal_referenceOSS(2);

            if (n->isTLControlled()) {
                GenerateJunctionControllerRecord(junctioncontrollerOSS, controllerID, 0);
                writeSignalReference(signal_referenceOSS, std::to_string(signalID), 0, 0, "-");
            }

            std::string centerMark = "none";
            const int inEdgeID = getID(inEdge->getID(), edgeMap, edgeID);
            // group parallel edges
            const NBEdge* outEdge = nullptr;
            bool isOuterEdge = true; // determine where a solid outer border should be drawn
            int lastFromLane = -1;
            std::vector<NBEdge::Connection> parallel;
            std::vector<NBEdge::Connection> connections = inEdge->getConnections();
            for (const NBEdge::Connection& c : connections) {
                // remove undesired self loops
                if (c.toEdge->getID() == ("-" + inEdge->getID()) ||
                    inEdge->getID() == ("-" + c.toEdge->getID())) {
                    continue;
                }
                assert(c.toEdge != 0);
                if (outEdge != c.toEdge || c.fromLane == lastFromLane) {
                    if (outEdge != nullptr) {
                        if (isOuterEdge) {
                            addPedestrianConnection(inEdge, outEdge, parallel);
                        }
                        // write road and record information for traffic light
                        OpenDRIVERoad road(getID(parallel.back().getInternalLaneID(), edgeMap, edgeID));
                        if (n->isTLControlled()) {
                            *(road.signals_device) << signal_referenceOSS.getString();
                        }
                        connectionID = writeInternalEdge(*(road.road_device), junction_intermediateOSS, inEdge, nID,
                                                         road.id,
                                                         inEdgeID,
                                                         getID(outEdge->getID(), edgeMap, edgeID),
                                                         connectionID,
                                                         parallel, isOuterEdge, straightThresh, centerMark,
                                                         signalLanes, 
                                                         road);
                        // get positions (lanes) where traffic light is having effect
                        for (RoadLane& lane : road.lanes) {
                            LanePoint& initial_point = lane.points[0];
                            Position tl_reference_position = initial_point.pos;
                            double heading = initial_point.heading;
                            traffic_light.references.emplace_back(TLReference{tl_reference_position, heading});
                        }
                        roads.emplace_back(road);
                        num_connections++;
                        num_connecting_roads++;
                        parallel.clear();
                        isOuterEdge = false;
                    }
                    outEdge = c.toEdge;
                }
                lastFromLane = c.fromLane;
                parallel.push_back(c);
            }
            if (isOuterEdge) {
                addPedestrianConnection(inEdge, outEdge, parallel);
            }
            if (!parallel.empty()) {
                if (!lefthand && (n->geometryLike() || inEdge->isTurningDirectionAt(outEdge))) {
                    centerMark = "solid";
                }
                // write road and record information for traffic light
                OpenDRIVERoad road(getID(parallel.back().getInternalLaneID(), edgeMap, edgeID));
                if (n->isTLControlled()) {
                    *(road.signals_device) << signal_referenceOSS.getString();
                }
                connectionID = writeInternalEdge(*(road.road_device), junction_intermediateOSS, inEdge, nID,
                                                 road.id,
                                                 inEdgeID,
                                                 getID(outEdge->getID(), edgeMap, edgeID),
                                                 connectionID,
                                                 parallel, isOuterEdge, straightThresh, centerMark,
                                                signalLanes, 
                                                road);
                // get positions (lanes) where traffic light is having effect
                for (RoadLane& lane : road.lanes) {
                    LanePoint& initial_point = lane.points[0];
                    Position tl_reference_position = initial_point.pos;
                    double heading = initial_point.heading;
                    traffic_light.references.emplace_back(TLReference{tl_reference_position, heading});
                }
                roads.emplace_back(road);
                num_connections++;
                num_connecting_roads++;
                parallel.clear();
            }
            // store controller record and traffic light
            if(n->isTLControlled() && num_connecting_roads > 0) {
                OutputDevice_String controllerOSS(2);
                GenerateControllerRecord(controllerOSS, controllerID, signalID,0);
                controllersOSS << controllerOSS.getString();
                traffic_lights.emplace_back(traffic_light);
            }
            ++signalID;
            ++controllerID;
        }

        // write the junction only of there are internal connections
        if (num_connections > 0) {
            junction_intermediateOSS << junctioncontrollerOSS.getString();
            junction_intermediateOSS << "    </junction>\n";
            junctionOSS << junction_intermediateOSS.getString();
            // calculate TL positions
            if (n->isTLControlled()) {
                Position maxPos = roads.back().lanes.back().points.back().pos;
                Position minPos = maxPos;
                int numpoints = 0;
                // compute junction bounding box
                for (auto& road : roads) {
                    for (RoadLane& lane : road.lanes) {
                        for (auto& point : lane.points) {
                            auto& pos = point.pos;
                            if(pos.x() > maxPos.x()) {
                                maxPos.setx(pos.x());
                            }
                            if(pos.y() > maxPos.y()) {
                                maxPos.sety(pos.y());
                            }
                            if(pos.x() < minPos.x()) {
                                minPos.setx(pos.x());
                            }
                            if(pos.y() < minPos.y()) {
                                minPos.sety(pos.y());
                            }
                            numpoints++;
                        }
                    }
                }
                // compute location for each traffic light
                for (auto& traffic_light : traffic_lights) {
                    double heading = 0;
                    // average position of the roads where the traffic light is having an effect
                    Position center = Position(0, 0, 0);
                    for (auto& reference : traffic_light.references) {
                        heading = heading + reference.heading;
                        center = center + reference.position;
                    }
                    heading = heading/traffic_light.references.size();
                    center = center/traffic_light.references.size();
                    // orientation of the roads where the traffic light is having an effect
                    Position orientation = Position(cos(heading), sin(heading), 0);
                    // direction perpendicular to the roads where the traffic light is affecting
                    Position lateral = Position(orientation.y(), -orientation.x(), 0);
                    // compute lateral displacement
                    double max_lateral_distance = -100;
                    for (auto& reference : traffic_light.references) {
                        double lateral_distance = lateral.dotProduct(reference.position - center);
                        if (lateral_distance > max_lateral_distance) {
                            max_lateral_distance = lateral_distance;
                        }
                    }
                    Position diagonal = maxPos - minPos;
                    double displacement = diagonal.length()*0.8;
                    // final position of the traffic light (moved forward and lateral from the point of effect)
                    Position traffic_light_position = center + orientation*displacement + lateral*(max_lateral_distance + 5);
                    OutputDevice_String signal_defOSS(2);
                    writeSignalInertial(signal_defOSS, std::to_string(traffic_light.id), "1000001",
                            traffic_light_position.x(), traffic_light_position.y(), traffic_light_position.z(),
                            heading, 0, 0);
                    auto& road = roads.front();
                    *(road.signals_device) << signal_defOSS.getString();
                }
            }
        }
        // write roads and signals to output device
        for (auto& road : roads) {
            *(road.road_device) << "        <signals>\n";
            *(road.road_device) << road.signals_device->getString();
            *(road.road_device) << "        </signals>\n";
            road.road_device->closeTag();
            device << road.road_device->getString();
        }
    }

    device.lf();
    device << controllersOSS.getString();
    device.lf();
    // write junctions (junction)
    device << junctionOSS.getString();

    // TODO: Useless??
    // for (std::map<std::string, NBNode*>::const_iterator i = nc.begin(); i != nc.end(); ++i) {
    //     NBNode* n = (*i).second;
    //     const std::vector<NBEdge*>& incoming = n->getIncomingEdges();
    //     // check if any connections must be written
    //     int numConnections = 0;
    //     for (std::vector<NBEdge*>::const_iterator j = incoming.begin(); j != incoming.end(); ++j) {
    //         numConnections += (int)((*j)->getConnections().size());
    //     }
    //     if (numConnections == 0) {
    //         continue;
    //     }
    //     for (std::vector<NBEdge*>::const_iterator j = incoming.begin(); j != incoming.end(); ++j) {
    //         const NBEdge* inEdge = *j;
    //         const std::vector<NBEdge::Connection>& elv = inEdge->getConnections();
    //         for (std::vector<NBEdge::Connection>::const_iterator k = elv.begin(); k != elv.end(); ++k) {
    //             const NBEdge::Connection& c = *k;
    //             const NBEdge* outEdge = c.toEdge;
    //             if (outEdge == nullptr) {
    //                 continue;
    //             }
    //         }
    //     }
    //     // write roads and signals to output device
    //     for (auto& road : roads) {
    //         *(road.road_device) << "        <signals>\n";
    //         *(road.road_device) << road.signals_device->getString();
    //         *(road.road_device) << "        </signals>\n";
    //         road.road_device->closeTag();
    //         device << road.road_device->getString();
    //     }
    // }

    // device.lf();
    // device << controllersOSS.getString();
    // device.lf();
    // write controllers
    for (std::map<std::string, NBNode*>::const_iterator i = nc.begin(); i != nc.end(); ++i) {
        NBNode* n = (*i).second;
        const std::vector<NBEdge*>& incoming = n->getIncomingEdges();
        // check if any connections must be written
        int numConnections = 0;
        for (std::vector<NBEdge*>::const_iterator j = incoming.begin(); j != incoming.end(); ++j) {
            numConnections += (int)((*j)->getConnections().size());
        }
        if (numConnections == 0) {
            continue;
        }
        for (std::vector<NBEdge*>::const_iterator j = incoming.begin(); j != incoming.end(); ++j) {
            const NBEdge* inEdge = *j;
            const std::vector<NBEdge::Connection>& elv = inEdge->getConnections();
            for (std::vector<NBEdge::Connection>::const_iterator k = elv.begin(); k != elv.end(); ++k) {
                const NBEdge::Connection& c = *k;
                const NBEdge* outEdge = c.toEdge;
                if (outEdge == nullptr) {
                    continue;
                }
            }
        }
    }

    device.closeTag();

    OptionsCont::getOptions().output_xodr_file = dynamic_cast<OutputDevice_String*>(&device)->getString();

    device.closeTag("OpenDRIVE");
    device.close();
}


std::string
NWWriter_OpenDrive::getDividerType(const NBEdge* e) {
    std::map<std::string, std::string> dividerTypeMapping;
    dividerTypeMapping["solid_line"] = "solid";
    dividerTypeMapping["dashed_line"] = "broken";
    dividerTypeMapping["double_solid_line"] = "solid solid";
    dividerTypeMapping["no"] = "none";

    // defaulting to solid as in the original code
    std::string dividerType = "solid";

    if (e->getParametersMap().count("divider") > 0) {
        std::string divider = e->getParametersMap().find("divider")->second;
        if (dividerTypeMapping.count(divider) > 0) {
            dividerType = dividerTypeMapping.find(divider)->second;
        }
    }
    return dividerType;
}


void
NWWriter_OpenDrive::writeNormalEdge(OutputDevice& device, const NBEdge* e,
                                    int edgeID, int fromNodeID, int toNodeID,
                                    const bool origNames,
                                    const double straightThresh,
                                    const ShapeContainer& shc,
                                    SignalLanes& signalLanes,
                                    const std::vector<std::string>& crossings) {
    // buffer output because some fields are computed out of order
    OutputDevice_String elevationOSS(3);
    elevationOSS.setPrecision(8);
    OutputDevice_String planViewOSS(2);
    planViewOSS.setPrecision(8);
    double length = 0;

    planViewOSS.openTag("planView");
    // for the shape we need to use the leftmost border of the leftmost lane
    const std::vector<NBEdge::Lane>& lanes = e->getLanes();
    PositionVector ls = getInnerLaneBorder(e);
#ifdef DEBUG_SMOOTH_GEOM
    if (DEBUGCOND) {
        std::cout << "write planview for edge " << e->getID() << "\n";
    }
#endif

    if (ls.size() == 2 || e->getPermissions() == SVC_PEDESTRIAN) {
        // foot paths may contain sharp angles
        length = writeGeomLines(ls, planViewOSS, elevationOSS);
    } else {
        bool ok = writeGeomSmooth(ls, e->getSpeed(), planViewOSS, elevationOSS, straightThresh, length);
        if (!ok) {
            WRITE_WARNINGF(TL("Could not compute smooth shape for edge '%'."), e->getID());
        }
    }
    planViewOSS.closeTag();

    device.openTag("road");
    device.writeAttr("name", StringUtils::escapeXML(e->getStreetName()));
    device.setPrecision(8); // length requires higher precision
    device.writeAttr("length", MAX2(POSITION_EPS, length));
    device.setPrecision(gPrecision);
    device.writeAttr("id", edgeID);
    device.writeAttr("junction", -1);
    if (fromNodeID != INVALID_ID || toNodeID != INVALID_ID) {
        device.openTag("link");
        if (fromNodeID != INVALID_ID) {
            device.openTag("predecessor");
            device.writeAttr("elementType", "junction");
            device.writeAttr("elementId", fromNodeID);
            device.closeTag();
        }
        if (toNodeID != INVALID_ID) {
            device.openTag("successor");
            device.writeAttr("elementType", "junction");
            device.writeAttr("elementId", toNodeID);
            device.closeTag();
        }
        device.closeTag();
    }
    device.openTag("type").writeAttr("s", 0).writeAttr("type", "town").closeTag();
    device << planViewOSS.getString();
    writeElevationProfile(ls, device, elevationOSS);
    device << "        <lateralProfile/>\n";
    device << "        <lanes>\n";
    device << "            <laneSection s=\"0\">\n";
    const std::string centerMark = e->getPermissions(e->getNumLanes() - 1) == 0 ? "none" : getDividerType(e);
    if (!LHLL) {
        writeEmptyCenterLane(device, centerMark, 0.13);
    }
    const std::string side = LHLL ? "left" : "right";
    device << "                <" << side << ">\n";
    const int numLanes = e->getNumLanes();
    for (int jRH = numLanes; --jRH >= 0;) {
        // XODR always has the lanes left to right (by default this is
        // inner-to-outer but in LH networks its outer-to-inner)
        const int j = lefthand ? numLanes - 1 - jRH : jRH;
        std::string laneType = e->getLaneStruct(j).type;
        if (laneType == "") {
            laneType = getLaneType(e->getPermissions(j));
        }
        device << "                    <lane id=\"" << s2x(j, numLanes) << "\" type=\"" << laneType << "\" level=\"true\">\n";
        device << "                        <link/>\n";
        // this could be used for geometry-link junctions without u-turn,
        // predecessor and sucessors would be lane indices,
        // road predecessor / succesfors would be of type 'road' rather than
        // 'junction'
        //device << "                            <predecessor id=\"-1\"/>\n";
        //device << "                            <successor id=\"-1\"/>\n";
        //device << "                        </link>\n";
        device << "                        <width sOffset=\"0\" a=\"" << e->getLaneWidth(j) << "\" b=\"0\" c=\"0\" d=\"0\"/>\n";
        std::string markType = "broken";
        if (LHRL) {
            if (j == numLanes - 1) {
                // solid road mark in the middle of the road
                markType = "solid";
            } else if ((e->getPermissions(j) & ~(SVC_PEDESTRIAN | SVC_BICYCLE)) == 0) {
                // solid road mark to the right of sidewalk or bicycle lane
                markType = "solid";
            }
        } else {
            if (j == 0) {
                markType = "solid";
            } else if (j > 0
                       && (e->getPermissions(j - 1) & ~(SVC_PEDESTRIAN | SVC_BICYCLE)) == 0) {
                // solid road mark to the left of sidewalk or bicycle lane
                markType = "solid";
            } else if (e->getPermissions(j) == 0) {
                // solid road mark to the right of a forbidden lane
                markType = "solid";
            }
        }
        std::string laneChange = "both";
        if (j == 0) {
            laneChange = "none";
        } else if (getLaneType(e->getPermissions(j - 1)) == laneType) {
            laneChange = "both";
        } else {
            laneChange = "none";
        }
        device << "                        <roadMark sOffset=\"0\" type=\"" << markType << "\" weight=\"standard\" color=\"standard\" width=\"0.13\" laneChange=\"" << laneChange << "\"/>\n";
        device << "                        <speed sOffset=\"0\" max=\"" << lanes[j].speed << "\"/>\n";
        device << "                    </lane>\n";
    }
    device << "                 </" << side << ">\n";
    if (LHLL) {
        writeEmptyCenterLane(device, centerMark, 0.13);
    }
    device << "            </laneSection>\n";
    device << "        </lanes>\n";
    writeRoadObjects(device, e, shc, crossings);
    if (origNames) {
        device << "        <userData code=\"sumoId\" value=\"" << e->getID() << "\"/>\n";
    }
    device.closeTag();
    checkLaneGeometries(e);
}

void
NWWriter_OpenDrive::addPedestrianConnection(const NBEdge* inEdge, const NBEdge* outEdge, std::vector<NBEdge::Connection>& parallel) {
    // by default there are no internal lanes for pedestrians. Determine if
    // one is feasible and does not exist yet.
    if (outEdge != nullptr
            && inEdge->getPermissions(0) == SVC_PEDESTRIAN
            && outEdge->getPermissions(0) == SVC_PEDESTRIAN
            && (parallel.empty()
                || parallel.front().fromLane != 0
                || parallel.front().toLane != 0)) {
        parallel.insert(parallel.begin(), NBEdge::Connection(0, const_cast<NBEdge*>(outEdge), 0));
        parallel.front().vmax = (inEdge->getLanes()[0].speed + outEdge->getLanes()[0].speed) / (double) 2.0;
    }
}


int
NWWriter_OpenDrive::writeInternalEdge(OutputDevice& device, OutputDevice& junctionDevice, const NBEdge* inEdge, int nodeID,
                                      int edgeID, int inEdgeID, int outEdgeID,
                                      int connectionID,
                                      const std::vector<NBEdge::Connection>& parallel,
                                      const bool isOuterEdge,
                                      const double straightThresh,
                                      const std::string& centerMark,
                                      SignalLanes& signalLanes,
                                      OpenDRIVERoad &discretizedRoad) {
    assert(parallel.size() != 0);
    const NBEdge::Connection& cLeft = LHRL ? parallel.front() : parallel.back();
    const NBEdge* outEdge = cLeft.toEdge;
    PositionVector begShape = getInnerLaneBorder(inEdge, cLeft.fromLane);
    PositionVector endShape = getInnerLaneBorder(outEdge, cLeft.toLane);
    //std::cout << "computing reference line for internal lane " << cLeft.getInternalLaneID() << " begLane=" << inEdge->getLaneShape(cLeft.fromLane) << " endLane=" << outEdge->getLaneShape(cLeft.toLane) << "\n";

    double length;
    double laneOffset = 0;
    PositionVector fallBackShape;
    fallBackShape.push_back(begShape.back());
    fallBackShape.push_back(endShape.front());
    const bool turnaround = inEdge->isTurningDirectionAt(outEdge);
    bool ok = true;
    PositionVector init = NBNode::bezierControlPoints(begShape, endShape, turnaround, 25, 25, ok, nullptr, straightThresh);
    if (init.size() == 0) {
        length = fallBackShape.length2D();
        // problem with turnarounds is known, method currently returns 'ok' (#2539)
        if (!ok) {
            WRITE_WARNINGF(TL("Could not compute smooth shape from lane '%' to lane '%'. Use option 'junctions.scurve-stretch' or increase radius of junction '%' to fix this."), inEdge->getLaneID(cLeft.fromLane), outEdge->getLaneID(cLeft.toLane), inEdge->getToNode()->getID());
        } else if (length <= NUMERICAL_EPS) {
            // left-curving geometry-like edges must use the right
            // side as reference line and shift
            begShape = getOuterLaneBorder(inEdge, cLeft.fromLane);
            endShape = getOuterLaneBorder(outEdge, cLeft.toLane);
            init = NBNode::bezierControlPoints(begShape, endShape, turnaround, 25, 25, ok, nullptr, straightThresh);
            if (init.size() != 0) {
                length = init.bezier(12).length2D();
                laneOffset = outEdge->getLaneWidth(cLeft.toLane);
                //std::cout << " internalLane=" << cLeft.getInternalLaneID() << " length=" << length << "\n";
            }
        }
    } else {
        length = init.bezier(12).length2D();
    }
    double roadLength = MAX2(POSITION_EPS, length);

    junctionDevice << "        <connection id=\"" << connectionID << "\" incomingRoad=\"" << inEdgeID << "\" connectingRoad=\"" << edgeID << "\" contactPoint=\"start\">\n";
    device.openTag("road");
    device.writeAttr("name", cLeft.id);
    device.setPrecision(8); // length requires higher precision
    device.writeAttr("length", roadLength);
    device.setPrecision(gPrecision);
    device.writeAttr("id", edgeID);
    device.writeAttr("junction", nodeID);
    device.openTag("link");
    device.openTag("predecessor");
    device.writeAttr("elementType", "road");
    device.writeAttr("elementId", inEdgeID);
    device.writeAttr("contactPoint", "end");
    device.closeTag();
    device.openTag("successor");
    device.writeAttr("elementType", "road");
    device.writeAttr("elementId", outEdgeID);
    device.writeAttr("contactPoint", "start");
    device.closeTag();
    device.closeTag();
    device.openTag("type").writeAttr("s", 0).writeAttr("type", "town").closeTag();
    device.openTag("planView");
    device.setPrecision(8); // geometry hdg requires higher precision
    OutputDevice_String elevationOSS(3);
    elevationOSS.setPrecision(8);
#ifdef DEBUG_SMOOTH_GEOM
    if (DEBUGCOND) {
        std::cout << "write planview for internal edge " << cLeft.id << " init=" << init << " fallback=" << fallBackShape
                  << " begShape=" << begShape << " endShape=" << endShape
                  << "\n";
    }
#endif
    // get road geometry
    std::vector<LanePoint> lane_0;
    if (init.size() == 0) {
        writeGeomLines(fallBackShape, device, elevationOSS);
        double heading = fallBackShape.angleAt2D(0);
        lane_0.emplace_back(LanePoint(0, fallBackShape[0], heading));
        lane_0.emplace_back(LanePoint(fallBackShape[0].distanceTo(fallBackShape[1]), fallBackShape[1], heading));
    } else {
        writeGeomPP3(device, elevationOSS, init, length);
        auto bezierpoints = init.bezier(2);
        double heading = init.angleAt2D(0);
        lane_0.emplace_back(LanePoint(0, bezierpoints[0], heading));
        heading = init.angleAt2D(init.size() - 2);
        lane_0.emplace_back(LanePoint(length, bezierpoints[1], heading));
    }
    device.setPrecision(gPrecision);
    device.closeTag();
    writeElevationProfile(fallBackShape, device, elevationOSS);
    device << "        <lateralProfile/>\n";
    device << "        <lanes>\n";
    if (laneOffset != 0) {
        device << "            <laneOffset s=\"0\" a=\"" << laneOffset << "\" b=\"0\" c=\"0\" d=\"0\"/>\n";
    }
    device << "            <laneSection s=\"0\">\n";
    if (!lefthand) {
        writeEmptyCenterLane(device, centerMark, 0);
    }
    const std::string side = lefthand ? "left" : "right";
    device << "                <" << side << ">\n";
    const int numLanes = (int)parallel.size();
    double accumulated_width = 0.0;
    for (int jRH = numLanes; --jRH >= 0;) {
        const int j = lefthand ? numLanes - 1 - jRH : jRH;
        const int xJ = s2x(j, numLanes);
        const NBEdge::Connection& c = parallel[j];
        const int fromIndex = s2x(c.fromLane, inEdge->getNumLanes());
        const int toIndex = s2x(c.toLane, outEdge->getNumLanes());

        double inEdgeWidth = inEdge->getLaneWidth(c.fromLane);
        double outEdgeWidth = outEdge->getLaneWidth(c.toLane);
        double lane_width = outEdge->getLaneWidth(c.toLane);
        // Ideally a polynomial function of third order would be needed for more precision.
        // This is obtained by specifying c and d coefficients, keeping it simple and linear
        // as we only know final and initial width.
        double bCoefficient = (outEdgeWidth - inEdgeWidth) / roadLength;
        double cCoefficient = 0;
        double dCoefficient = 0;
        device << "                    <lane id=\"" << xJ << "\" type=\"" << getLaneType(outEdge->getPermissions(c.toLane)) << "\" level=\"true\">\n";
        device << "                        <link>\n";
        device << "                            <predecessor id=\"" << fromIndex << "\"/>\n";
        device << "                            <successor id=\"" << toIndex << "\"/>\n";
        device << "                        </link>\n";
        device << "                        <width sOffset=\"0\" a=\"" << inEdgeWidth << "\" b=\"" << bCoefficient << "\" c=\"" << cCoefficient << "\" d=\"" << dCoefficient << "\"/>\n";
        std::string markType = "broken";
        if (inEdge->isTurningDirectionAt(outEdge)) {
            markType = "none";
        } else if (c.fromLane == 0 && c.toLane == 0 && isOuterEdge) {
            // solid road mark at the outer border
            markType = "solid";
        } else if (isOuterEdge && j > 0
                   && (outEdge->getPermissions(parallel[j - 1].toLane) & ~(SVC_PEDESTRIAN | SVC_BICYCLE)) == 0) {
            // solid road mark to the left of sidewalk or bicycle lane
            markType = "solid";
        } else if (!inEdge->getToNode()->geometryLike()) {
            // draw shorter road marks to indicate turning paths
            LinkDirection dir = inEdge->getToNode()->getDirection(inEdge, outEdge, lefthand);
            if (dir == LinkDirection::LEFT || dir == LinkDirection::RIGHT || dir == LinkDirection::PARTLEFT || dir == LinkDirection::PARTRIGHT) {
                // XXX <type><line/><type> is not rendered by odrViewer so cannot be validated
                // device << "                             <type name=\"broken\" width=\"0.13\">\n";
                // device << "                                  <line length=\"0.5\" space=\"0.5\" tOffset=\"0\" sOffset=\"0\" rule=\"none\"/>\n";
                // device << "                             </type>\n";
                markType = "none";
            }
        }
        device << "                        <roadMark sOffset=\"0\" type=\"" << markType << "\" weight=\"standard\" color=\"standard\" width=\"0.13\"/>\n";
        device << "                        <speed sOffset=\"0\" max=\"" << c.vmax << "\"/>\n";
        device << "                    </lane>\n";

        junctionDevice << "            <laneLink from=\"" << fromIndex << "\" to=\"" << xJ << "\"/>\n";
        // const int toLane = -(parallel.size() - j);
        // junctionDevice << "            <laneLink from=\"" << fromIndex << "\" to=\"" << toLane << "\"/>\n";
        connectionID++;
        // get lane geometry
        
        if (getLaneType(outEdge->getPermissions(c.toLane)) == "driving") {
            RoadLane lane;
            double displacement = accumulated_width + lane_width*0.5;
            for (LanePoint& point : lane_0) {
                double heading = point.heading;
                Position lane_position = point.pos + Position(sin(heading), -cos(heading), 0) * displacement;
                lane.points.emplace_back(LanePoint(point.s, lane_position, heading));
            }
            discretizedRoad.lanes.emplace_back(lane);
        }
        accumulated_width += lane_width;
    }
    device << "                 </" << side << ">\n";
    if (lefthand) {
        writeEmptyCenterLane(device, centerMark, 0);
    }
    device << "            </laneSection>\n";
    device << "        </lanes>\n";
    device << "        <objects/>\n";
    UNUSED_PARAMETER(signalLanes);
    device << "        <signals/>\n";
    device.closeTag();
    junctionDevice << "        </connection>\n";

    return connectionID;
}


double
NWWriter_OpenDrive::writeGeomLines(const PositionVector& shape, OutputDevice& device, OutputDevice& elevationDevice, double offset) {
    for (int j = 0; j < (int)shape.size() - 1; ++j) {
        const Position& p = shape[j];
        const Position& p2 = shape[j + 1];
        const double hdg = shape.angleAt2D(j);
        const double length = p.distanceTo2D(p2);
        device.openTag("geometry");
        device.writeAttr("s", offset);
        device.writeAttr("x", p.x());
        device.writeAttr("y", p.y());
        device.writeAttr("hdg", hdg);
        device.writeAttr("length", length < 1e-8 ? 1e-8 : length);
        device.openTag("line").closeTag();
        device.closeTag();
        elevationDevice << "            <elevation s=\"" << offset << "\" a=\"" << p.z() << "\" b=\"" << (p2.z() - p.z()) / MAX2(POSITION_EPS, length) << "\" c=\"0\" d=\"0\"/>\n";
        offset += length;
    }
    return offset;
}


void
NWWriter_OpenDrive::writeEmptyCenterLane(OutputDevice& device, const std::string& mark, double markWidth) {
    device << "                <center>\n";
    device << "                    <lane id=\"0\" type=\"none\" level=\"true\">\n";
    device << "                        <link/>\n";
    // laneChange = none as roads contain lanes in one direction only
    device << "                        <roadMark sOffset=\"0\" type=\"" << mark << "\" weight=\"standard\" color=\"standard\" width=\"" << markWidth << "\" laneChange=\"none\"/>\n";
    device << "                    </lane>\n";
    device << "                </center>\n";
}


int
NWWriter_OpenDrive::getID(const std::string& origID, StringBijection<int>& map, int& lastID) {
    if (map.hasString(origID)) {
        return map.get(origID);
    }
    map.insert(origID, lastID++);
    return lastID - 1;
}


std::string
NWWriter_OpenDrive::getLaneType(SVCPermissions permissions) {
    switch (permissions) {
        case SVC_PEDESTRIAN:
            return "sidewalk";
        //case (SVC_BICYCLE | SVC_PEDESTRIAN):
        //    WRITE_WARNING("Ambiguous lane type (biking+driving) for road '" + roadID + "'");
        //    return "sidewalk";
        case SVC_BICYCLE:
            return "biking";
        case 0:
            // ambiguous
            return "none";
        case SVC_RAIL:
        case SVC_RAIL_URBAN:
        case SVC_RAIL_ELECTRIC:
        case SVC_RAIL_FAST:
            return "rail";
        case SVC_TRAM:
            return "tram";
        default: {
            // complex permissions
            if (permissions == SVCAll) {
                return "driving";
            } else if (isRailway(permissions)) {
                return "rail";
            } else if ((permissions & SVC_PASSENGER) != 0) {
                return "driving";
            } else {
                return "restricted";
            }
        }
    }
}


PositionVector
NWWriter_OpenDrive::getInnerLaneBorder(const NBEdge* edge, int laneIndex, double widthOffset) {
    if (laneIndex == -1) {
        // innermost lane
        laneIndex = (int)edge->getNumLanes() - 1;
        if (LHRL) {
            laneIndex = 0;
        }
    }
    PositionVector result = edge->getLaneShape(laneIndex);
    widthOffset -= (LHLL ? -1 : 1) * edge->getLaneWidth(laneIndex) / 2;
    try {
        result.move2side(widthOffset);
    } catch (InvalidArgument&) { }
    return result;
}


PositionVector
NWWriter_OpenDrive::getOuterLaneBorder(const NBEdge* edge, int laneIndex) {
    return getInnerLaneBorder(edge, laneIndex, edge->getLaneWidth(laneIndex));
}


double
NWWriter_OpenDrive::writeGeomPP3(
    OutputDevice& device,
    OutputDevice& elevationDevice,
    PositionVector init,
    double length,
    double offset) {
    assert(init.size() == 3 || init.size() == 4);

    // avoid division by 0
    length = MAX2(POSITION_EPS, length);

    const Position p = init.front();
    const double hdg = init.angleAt2D(0);

    // backup elevation values
    const PositionVector initZ = init;
    // translate to u,v coordinates
    init.add(-p.x(), -p.y(), -p.z());
    init.rotate2D(-hdg);

    // parametric coefficients
    double aU, bU, cU, dU;
    double aV, bV, cV, dV;
    double aZ, bZ, cZ, dZ;

    // unfactor the Bernstein polynomials of degree 2 (or 3) and collect the coefficients
    if (init.size() == 3) {
        //f(x, a, b ,c) = a + (2*b - 2*a)*x + (a - 2*b + c)*x*x
        aU = init[0].x();
        bU = 2 * init[1].x() - 2 * init[0].x();
        cU = init[0].x() - 2 * init[1].x() + init[2].x();
        dU = 0;

        aV = init[0].y();
        bV = 2 * init[1].y() - 2 * init[0].y();
        cV = init[0].y() - 2 * init[1].y() + init[2].y();
        dV = 0;

        // elevation is not parameteric on [0:1] but on [0:length]
        aZ = initZ[0].z();
        bZ = (2 * initZ[1].z() - 2 * initZ[0].z()) / length;
        cZ = (initZ[0].z() - 2 * initZ[1].z() + initZ[2].z()) / (length * length);
        dZ = 0;

    } else {
        // f(x, a, b, c, d) = a + (x*((3*b) - (3*a))) + ((x*x)*((3*a) + (3*c) - (6*b))) + ((x*x*x)*((3*b) - (3*c) - a + d))
        aU = init[0].x();
        bU = 3 * init[1].x() - 3 * init[0].x();
        cU = 3 * init[0].x() - 6 * init[1].x() + 3 * init[2].x();
        dU = -init[0].x() + 3 * init[1].x() - 3 * init[2].x() + init[3].x();

        aV = init[0].y();
        bV = 3 * init[1].y() - 3 * init[0].y();
        cV = 3 * init[0].y() - 6 * init[1].y() + 3 * init[2].y();
        dV = -init[0].y() + 3 * init[1].y() - 3 * init[2].y() + init[3].y();

        // elevation is not parameteric on [0:1] but on [0:length]
        aZ = initZ[0].z();
        bZ = (3 * initZ[1].z() - 3 * initZ[0].z()) / length;
        cZ = (3 * initZ[0].z() - 6 * initZ[1].z() + 3 * initZ[2].z()) / (length * length);
        dZ = (-initZ[0].z() + 3 * initZ[1].z() - 3 * initZ[2].z() + initZ[3].z()) / (length * length * length);
    }

    device.openTag("geometry");
    device.writeAttr("s", offset);
    device.writeAttr("x", p.x());
    device.writeAttr("y", p.y());
    device.writeAttr("hdg", hdg);
    device.writeAttr("length", length);

    device.openTag("paramPoly3");
    device.writeAttr("aU", aU);
    device.writeAttr("bU", bU);
    device.writeAttr("cU", cU);
    device.writeAttr("dU", dU);
    device.writeAttr("aV", aV);
    device.writeAttr("bV", bV);
    device.writeAttr("cV", cV);
    device.writeAttr("dV", dV);
    device.writeAttr("pRange", "normalized");
    device.closeTag();
    device.closeTag();

    // write elevation
    elevationDevice.openTag("elevation");
    elevationDevice.writeAttr("s", offset);
    elevationDevice.writeAttr("a", aZ);
    elevationDevice.writeAttr("b", bZ);
    elevationDevice.writeAttr("c", cZ);
    elevationDevice.writeAttr("d", dZ);
    elevationDevice.closeTag();

    return offset + length;
}


bool
NWWriter_OpenDrive::writeGeomSmooth(const PositionVector& shape, double speed, OutputDevice& device, OutputDevice& elevationDevice, double straightThresh, double& length) {
#ifdef DEBUG_SMOOTH_GEOM
    if (DEBUGCOND) {
        std::cout << "writeGeomSmooth\n  n=" << shape.size() << " shape=" << toString(shape) << "\n";
    }
#endif
    bool ok = true;
    const double longThresh = speed; //  16.0; // make user-configurable (should match the sampling rate of the source data)
    const double curveCutout = longThresh / 2; // 8.0; // make user-configurable (related to the maximum turning rate)
    // the length of the segment that is added for cutting a corner can be bounded by 2*curveCutout (prevent the segment to be classified as 'long')
    assert(longThresh >= 2 * curveCutout);
    assert(shape.size() > 2);
    // add intermediate points wherever there is a strong angular change between long segments
    // assume the geometry is simplified so as not to contain consecutive colinear points
    PositionVector shape2 = shape;
    double maxAngleDiff = 0;
    double offset = 0;
    for (int j = 1; j < (int)shape.size() - 1; ++j) {
        //const double hdg = shape.angleAt2D(j);
        const Position& p0 = shape[j - 1];
        const Position& p1 = shape[j];
        const Position& p2 = shape[j + 1];
        const double dAngle = fabs(GeomHelper::angleDiff(p0.angleTo2D(p1), p1.angleTo2D(p2)));
        const double length1 = p0.distanceTo2D(p1);
        const double length2 = p1.distanceTo2D(p2);
        maxAngleDiff = MAX2(maxAngleDiff, dAngle);
#ifdef DEBUG_SMOOTH_GEOM
        if (DEBUGCOND) {
            std::cout << "   j=" << j << " dAngle=" << RAD2DEG(dAngle) << " length1=" << length1 << " length2=" << length2 << "\n";
        }
#endif
        if (dAngle > straightThresh
                && (length1 > longThresh || j == 1)
                && (length2 > longThresh || j == (int)shape.size() - 2)) {
            // NBNode::bezierControlPoints checks for minimum length of POSITION_EPS so we make sure there is no instability
            shape2.insertAtClosest(shape.positionAtOffset2D(offset + length1 - MIN2(length1 - 2 * POSITION_EPS, curveCutout)), false);
            shape2.insertAtClosest(shape.positionAtOffset2D(offset + length1 + MIN2(length2 - 2 * POSITION_EPS, curveCutout)), false);
            shape2.removeClosest(p1);
        }
        offset += length1;
    }
    const int numPoints = (int)shape2.size();
#ifdef DEBUG_SMOOTH_GEOM
    if (DEBUGCOND) {
        std::cout << " n=" << numPoints << " shape2=" << toString(shape2) << "\n";
    }
#endif

    if (maxAngleDiff < straightThresh) {
        length = writeGeomLines(shape2, device, elevationDevice, 0);
#ifdef DEBUG_SMOOTH_GEOM
        if (DEBUGCOND) {
            std::cout << "   special case: all lines. maxAngleDiff=" << maxAngleDiff << "\n";
        }
#endif
        return ok;
    }

    // write the long segments as lines, short segments as curves
    offset = 0;
    for (int j = 0; j < numPoints - 1; ++j) {
        const Position& p0 = shape2[j];
        const Position& p1 = shape2[j + 1];
        PositionVector line;
        line.push_back(p0);
        line.push_back(p1);
        const double lineLength = line.length2D();
        if (lineLength >= longThresh) {
            offset = writeGeomLines(line, device, elevationDevice, offset);
#ifdef DEBUG_SMOOTH_GEOM
            if (DEBUGCOND) {
                std::cout << "      writeLine=" << toString(line) << "\n";
            }
#endif
        } else {
            // find control points
            PositionVector begShape;
            PositionVector endShape;
            if (j == 0 || j == numPoints - 2) {
                // keep the angle of the first/last segment but end at the front of the shape
                begShape = line;
                begShape.add(p0 - begShape.back());
            } else if (j == 1 || p0.distanceTo2D(shape2[j - 1]) > longThresh) {
                // use the previous segment if it is long or the first one
                begShape.push_back(shape2[j - 1]);
                begShape.push_back(p0);
            } else {
                // end at p0 with mean angle of the previous and current segment
                begShape.push_back(shape2[j - 1]);
                begShape.push_back(p1);
                begShape.add(p0 - begShape.back());
            }

            if (j == 0 || j == numPoints - 2) {
                // keep the angle of the first/last segment but start at the end of the shape
                endShape = line;
                endShape.add(p1 - endShape.front());
            } else if (j == numPoints - 3 || p1.distanceTo2D(shape2[j + 2]) > longThresh) {
                // use the next segment if it is long or the final one
                endShape.push_back(p1);
                endShape.push_back(shape2[j + 2]);
            } else {
                // start at p1 with mean angle of the current and next segment
                endShape.push_back(p0);
                endShape.push_back(shape2[j + 2]);
                endShape.add(p1 - endShape.front());
            }
            const double extrapolateLength = MIN2((double)25, lineLength / 4);
            PositionVector init = NBNode::bezierControlPoints(begShape, endShape, false, extrapolateLength, extrapolateLength, ok, nullptr, straightThresh);
            if (init.size() == 0) {
                // could not compute control points, write line
                offset = writeGeomLines(line, device, elevationDevice, offset);
#ifdef DEBUG_SMOOTH_GEOM
                if (DEBUGCOND) {
                    std::cout << "      writeLine lineLength=" << lineLength << " begShape" << j << "=" << toString(begShape) << " endShape" << j << "=" << toString(endShape) << " init" << j << "=" << toString(init) << "\n";
                }
#endif
            } else {
                // write bezier
                const double curveLength = init.bezier(12).length2D();
                offset = writeGeomPP3(device, elevationDevice, init, curveLength, offset);
#ifdef DEBUG_SMOOTH_GEOM
                if (DEBUGCOND) {
                    std::cout << "      writeCurve lineLength=" << lineLength << " curveLength=" << curveLength << " begShape" << j << "=" << toString(begShape) << " endShape" << j << "=" << toString(endShape) << " init" << j << "=" << toString(init) << "\n";
                }
#endif
            }
        }
    }
    length = offset;
    return ok;
}


void
NWWriter_OpenDrive::writeElevationProfile(const PositionVector& shape, OutputDevice& device, const OutputDevice_String& elevationDevice) {
    // check if the shape is flat
    bool flat = true;
    double z = shape.size() == 0 ? 0 : shape[0].z();
    for (int i = 1; i < (int)shape.size(); ++i) {
        if (fabs(shape[i].z() - z) > NUMERICAL_EPS) {
            flat = false;
            break;
        }
    }
    device << "        <elevationProfile>\n";
    if (flat) {
        device << "            <elevation s=\"0\" a=\"" << z << "\" b=\"0\" c=\"0\" d=\"0\"/>\n";
    } else {
        device << elevationDevice.getString();
    }
    device << "        </elevationProfile>\n";

}


void
NWWriter_OpenDrive::checkLaneGeometries(const NBEdge* e) {
    if (e->getNumLanes() > 1) {
        // compute 'stop line' of rightmost lane
        const PositionVector shape0 = e->getLaneShape(0);
        assert(shape0.size() >= 2);
        const Position& from = shape0[-2];
        const Position& to = shape0[-1];
        PositionVector stopLine;
        stopLine.push_back(to);
        stopLine.push_back(to - PositionVector::sideOffset(from, to, lefthand ? 1000 : -1000));
        // endpoints of all other lanes should be on the stop line
        for (int lane = 1; lane < e->getNumLanes(); ++lane) {
            const double dist = stopLine.distance2D(e->getLaneShape(lane)[-1]);
            if (dist > NUMERICAL_EPS) {
                WRITE_WARNINGF(TL("Uneven stop line at lane '%' (dist=%) cannot be represented in OpenDRIVE."), e->getLaneID(lane), toString(dist));
            }
        }
    }
}

void
NWWriter_OpenDrive::writeRoadObjects(OutputDevice& device, const NBEdge* e, const ShapeContainer& shc, const std::vector<std::string>& crossings) {
    device.openTag("objects");
    if (e->hasParameter(ROAD_OBJECTS)) {
        device.setPrecision(8); // geometry hdg requires higher precision
        PositionVector road = getInnerLaneBorder(e);
        for (std::string id : StringTokenizer(e->getParameter(ROAD_OBJECTS, "")).getVector()) {
            SUMOPolygon* p = shc.getPolygons().get(id);
            if (p == nullptr) {
                PointOfInterest* poi = shc.getPOIs().get(id);
                if (poi == nullptr) {
                    WRITE_WARNINGF("Road object polygon or POI '%' not found for edge '%'", id, e->getID());
                } else {
                    writeRoadObjectPOI(device, e, road, poi);
                }
            } else {
                writeRoadObjectPoly(device, e, road, p);
            }
        }
        device.setPrecision(gPrecision);
    }
    if (crossings.size() > 0) {
        device.setPrecision(8); // geometry hdg requires higher precision
        PositionVector road = getInnerLaneBorder(e);
        for (size_t ic = 0; ic < crossings.size(); ic++) {
            SUMOPolygon* p = shc.getPolygons().get(crossings[ic]);
            if (p != 0) {
                writeRoadObjectPoly(device, e, road, p);
            }
        }
        device.setPrecision(gPrecision);
    }
    device.closeTag();
}


std::vector<NWWriter_OpenDrive::TrafficSign>
NWWriter_OpenDrive::parseTrafficSign(const std::string& trafficSign, PointOfInterest* poi) {
    std::vector<TrafficSign> result;
    // check for maxspeed, stop, give_way and hazard
    if (trafficSign == "maxspeed" && poi->hasParameter("maxspeed")) {
        result.push_back(TrafficSign{ "OpenDrive", "maxspeed", "", poi->getParameter("maxspeed")});
    } else if (trafficSign == "stop") {
        result.push_back(TrafficSign{ "OpenDrive", trafficSign, "", "" });
    } else if (trafficSign == "give_way") {
        result.push_back(TrafficSign{ "OpenDrive", trafficSign, "", "" });
    } else if (trafficSign == "hazard" && poi->hasParameter("hazard")) {
        result.push_back(TrafficSign{ "OpenDrive", trafficSign, poi->getParameter("hazard"), "" });
    } else {
        if (trafficSign.find_first_of(",;") != std::string::npos) {
            std::string::size_type colon = trafficSign.find(':');
            const std::string country = trafficSign.substr(0, colon);
            const std::string remaining = trafficSign.substr(colon + 1);

            std::vector<std::string> tokens;
            std::string::size_type lastPos = 0;
            std::string::size_type pos = remaining.find_first_of(",;");
            while (pos != std::string::npos) {
                // add country and colon before pushing the token
                tokens.push_back(country + ":" + remaining.substr(lastPos, pos - lastPos));
                lastPos = pos + 1;
                pos = remaining.find_first_of(",;", lastPos);
            }
            tokens.push_back(country + ":" + remaining.substr(lastPos));
            // call for each token the parseTrafficSignId function and fill the result vector
            for (std::string token : tokens) {
                result.push_back(parseTrafficSignId(token));
            }
        } else {
            result.push_back(parseTrafficSignId(trafficSign));
        }
    }
    return result;
}


NWWriter_OpenDrive::TrafficSign
NWWriter_OpenDrive::parseTrafficSignId(const std::string& trafficSign) {
    // for OpenDrive 1.4 the country code is specified as ISO 3166-1, alpha-3,
    // but OSM uses ISO 3166-1, alpha-2. In order to be OpenDrive 1.4 compliant
    // we would need to convert it back to alpha-3. However, newer OpenDrive
    // versions support alpha-2, so we use that instead.

    //std::regex re("([A-Z]{2}):([0-9]{1,3})(?:\\[([0-9]{1,3})\\])?");

    // This regex is used to parse the traffic sign id. It matches the following groups:
    // 1. country code (2 letters)
    // 2. sign id (1-4 digits) e.g. 1020 or 274.1
    // 3. value in brackets e.g. 1020[50] or 274.1[50] -> 50
    // 4. value after the hyphen 1020-50 or 274.1-50 -> 50
    std::regex re("([A-Z]{2}):([0-9.]{1,6}|[A-Z]{1}[0-9.]{2,6})(?:\\[(.*)\\])?(?:-(.*))?");
    std::smatch match;
    std::regex_match(trafficSign, match, re);
    if (match.size() == 5) {
        std::string value;
        if (match[3].matched) {
            value = match[3].str();
        } else if (match[4].matched) {
            value = match[4].str();
        }
        return TrafficSign{ match[1], match[2], "", value};
    } else {
        return TrafficSign{ "OpenDrive", trafficSign, "", ""};
    }
}

// void 
// NWWriter_OpenDrive::writeSignal(OutputDevice& device, std::string id, double s, double t, std::string type, double hOffset) {
//     device.openTag("signal");
//     device.writeAttr("id", id);
//     device.writeAttr("s", s);
//     device.writeAttr("t", t);
//     device.writeAttr("name", "signal:"+id);
//     device.writeAttr("dynamic", "yes");
//     device.writeAttr("orientation", "none");
//     device.writeAttr("zOffset", 0);
//     device.writeAttr("country", "OpenDRIVE");
//     device.writeAttr("type", type); // "1000001" is traffic light
//     device.writeAttr("subtype", "-1");
//     device.writeAttr("value", -1);
//     device.writeAttr("height", 0);
//     device.writeAttr("width", 0.5);
//     device.writeAttr("text", "");
//     device.writeAttr("hOffset", hOffset);
//     device.writeAttr("pitch", 0);
//     device.writeAttr("roll", 0);
//     device.openTag("validity");
//     device.writeAttr("fromLane", 0);
//     device.writeAttr("toLane", 0);
//     device.closeTag();
//     device.closeTag();
// }

// void
// NWWriter_OpenDrive::writeSignalInertial(
//         OutputDevice& device, std::string id, std::string type,
//         double x, double y, double z, 
//         double hdg, double pitch, double roll) {
//     device.openTag("signal");
//     device.writeAttr("id", id);
//     device.writeAttr("s", 0);
//     device.writeAttr("t", 0);
//     device.writeAttr("name", "signal:"+id);
//     device.writeAttr("dynamic", "yes");
//     device.writeAttr("orientation", "none");
//     device.writeAttr("zOffset", 0);
//     device.writeAttr("country", "OpenDRIVE");
//     device.writeAttr("type", type); // "1000001" is traffic light
//     device.writeAttr("subtype", "-1");
//     device.writeAttr("value", -1);
//     device.writeAttr("height", 0);
//     device.writeAttr("width", 0.5);
//     device.writeAttr("text", "");
//     device.writeAttr("hOffset", 0);
//     device.writeAttr("pitch", 0);
//     device.writeAttr("roll", 0);
//     device.openTag("validity");
//     device.writeAttr("fromLane", 0);
//     device.writeAttr("toLane", 0);
//     device.closeTag();
//     device.openTag("positionInertial");
//     device.writeAttr("x", x);
//     device.writeAttr("y", y);
//     device.writeAttr("z", z);
//     device.writeAttr("hdg", hdg);
//     device.writeAttr("pitch", pitch);
//     device.writeAttr("roll", roll);
//     device.closeTag();
//     device.closeTag();
// }


// void
// NWWriter_OpenDrive::writeSignalReference(OutputDevice& device, std::string id, double s, double t, std::string orientation) {
//     device.openTag("signalReference");
//     device.writeAttr("id", id);
//     device.writeAttr("s", s);
//     device.writeAttr("t", t);
//     device.writeAttr("orientation", orientation);
//     device.closeTag();
// }

// std::pair<double, double>
// NWWriter_OpenDrive::ComputePointSegmentDistance(
//             const Position& P1, const Position& P2, const Position& P3) {
//     Position P1P2 = P1 - P2;
//     double DP1P2 = P1P2.dotProduct(P1P2);
//     double t = (P3 - P1).dotProduct(P1P2) / DP1P2;
//     double dist = P3.distanceTo(P1 - P1P2*t);
//     return std::make_pair(t, dist);
// }

// void 
// NWWriter_OpenDrive::GenerateJunctionControllerRecord(OutputDevice& device, int controllerID, int sequence) {
//     device.openTag("controller");
//     device.writeAttr("id", controllerID);
//     device.writeAttr("type", 0);
//     device.writeAttr("sequence", sequence);
//     device.closeTag();
// }

void
NWWriter_OpenDrive::writeSignals(OutputDevice& device, const NBEdge* e, double length,
                                 SignalLanes& signalLanes, const ShapeContainer& shc) {
    device.openTag("signals");
    if (e->getToNode()->isTLControlled()) {
        // try to faithfully represent the SUMO signal layout
        // (if a realistic number of signals is needed, the user should set
        // option --tls.group-signals)
        // IDs to ints tip staticunordered map <std:string, uint8>
        // Checkear sealkes importer
        NBTrafficLightDefinition* tl = *e->getToNode()->getControllingTLS().begin();
        std::map<std::string, bool> toWrite;
        for (const NBConnection& c : tl->getControlledLinks()) {
            if (c.getFrom() == e) {
                const std::string id = std::to_string(getintID(tl->getID() + "_" + toString(c.getTLIndex())));
                if (toWrite.count(id) == 0) {
                    toWrite[id] = signalLanes.count(id) == 0;
                }
                signalLanes[id].first.insert(c.getFromLane());
                signalLanes[id].second.insert(e->getToNode()->getDirection(e, c.getTo()));
            }
        }
        for (auto item : toWrite) {
            const std::string id = item.first;
            const bool isNew = item.second;
            const std::set<LinkDirection>& dirs = signalLanes[id].second;
            const bool l = dirs.count(LinkDirection::LEFT) != 0 || dirs.count(LinkDirection::PARTLEFT) != 0;
            const bool r = dirs.count(LinkDirection::RIGHT) != 0 || dirs.count(LinkDirection::PARTRIGHT) != 0;
            const bool s = dirs.count(LinkDirection::STRAIGHT) != 0;
            const std::string tag = isNew ? "signal" : "signalReference";
            int firstLane = *signalLanes[id].first.begin();
            double t = e->getLaneWidth(firstLane) * 0.5;
            for (int i = firstLane + 1; i < e->getNumLanes(); i++) {
                t += e->getLaneWidth(i);
            }
            device.openTag(tag);
            device.writeAttr("id", id);
            device.setPrecision(8);
            device.writeAttr("s", length);
            device.writeAttr("t", -t);
            device.writeAttr("orientation", "+");
            if (isNew) {
                int type = 1000001;
                int subType = -1;
                if (l && !s && !r) {
                    type = 1000011;
                    subType = 10;
                } else if (!l && !s && r) {
                    type = 1000011;
                    subType = 20;
                } else if (!l && s && !r) {
                    type = 1000011;
                    subType = 30;
                } else if (l && s && !r) {
                    type = 1000011;
                    subType = 40;
                } else if (!l && s && r) {
                    type = 1000011;
                    subType = 50;
                }
                device.writeAttr("dynamic", "yes");
                device.writeAttr("zOffset", 5);
                device.writeAttr("country", "OpenDRIVE");
                device.writeAttr("type", type);
                device.writeAttr("subtype", subType);
                device.writeAttr("height", 0.78);
                device.writeAttr("width", 0.26);
            }
            for (int lane : signalLanes[id].first) {
                if(lane > 0)
                  lane = 0 - lane;
                device.openTag("validity");
                device.writeAttr("fromLane", s2x(lane, e->getNumLanes()));
                device.writeAttr("toLane", s2x(lane, e->getNumLanes()));
                device.closeTag();
            }
            device.closeTag();
        }
    } else if (e->hasParameter(ROAD_OBJECTS)) {
        PositionVector roadShape = getInnerLaneBorder(e);
        for (std::string id : StringTokenizer(e->getParameter(ROAD_OBJECTS, "")).getVector()) {
            PointOfInterest* poi = shc.getPOIs().get(id);
            if (poi != nullptr) {
                if (poi->getShapeType() == "traffic_sign" && poi->hasParameter("traffic_sign")) {
                    std::string traffic_sign_type = poi->getParameter("traffic_sign");

                    std::vector<TrafficSign> trafficSigns = parseTrafficSign(traffic_sign_type, poi);

                    auto distance = roadShape.nearest_offset_to_point2D(*poi, true);
                    double t = getRoadSideOffset(e);
                    double calculatedZOffset = 3.0;
                    for (auto it = trafficSigns.rbegin(); it != trafficSigns.rend(); ++it) {
                        TrafficSign trafficSign = *it;
                        device.openTag("signal");
                        device.writeAttr("id", id);
                        device.writeAttr("s", distance);
                        device.writeAttr("t", t);
                        device.writeAttr("orientation", "-");
                        device.writeAttr("dynamic", "no");
                        device.writeAttr("zOffset", calculatedZOffset);
                        device.writeAttr("country", trafficSign.country);
                        device.writeAttr("type", trafficSign.type);
                        device.writeAttr("subtype", trafficSign.subtype);
                        device.writeAttr("value", trafficSign.value);
                        device.writeAttr("height", 0.78);
                        device.writeAttr("width", 0.78);
                        device.closeTag();
                        calculatedZOffset += 0.78;
                    }
                }
            }
        }
    }
    device.closeTag();
}


double
NWWriter_OpenDrive::getRoadSideOffset(const NBEdge* e) {
    double t = 0.30;
    if (!lefthand || LHLL) {
        for (int i = 0; i < e->getNumLanes(); i++) {
            t += e->getPermissions(i) == SVC_PEDESTRIAN ? e->getLaneWidth(i) * 0.2 : e->getLaneWidth(i);
        }
    }
    t = LHRL ? t : -t;
    return t;
}


int
NWWriter_OpenDrive::s2x(int sumoIndex, int numLanes) {
    // sumo lanes:     0, 1, 2  (0 being the outermost lane)
    // XODR:          -3,-2,-1  (written in reverse order)
    // LHLL:           3, 2, 1  (written in reverse order)
    // lefthand (old):-1,-2,-3
    return (lefthand
            ? (LHLL
               ? numLanes - sumoIndex
               : - sumoIndex - 1)
            : sumoIndex - numLanes);
}


void
NWWriter_OpenDrive::mapmatchRoadObjects(const ShapeContainer& shc,  const NBEdgeCont& ec) {
    if (shc.getPolygons().size() == 0 && shc.getPOIs().size() == 0) {
        return;
    }
    const double maxDist = OptionsCont::getOptions().getFloat("opendrive-output.shape-match-dist");
    if (maxDist < 0) {
        return;
    }
    // register custom assignements
    std::set<std::string> assigned;
    for (auto it = ec.begin(); it != ec.end(); ++it) {
        NBEdge* e = it->second;
        if (e->hasParameter(ROAD_OBJECTS)) {
            for (std::string id : StringTokenizer(e->getParameter(ROAD_OBJECTS, "")).getVector()) {
                assigned.insert(id);
            }
        }
    }
    // build rtree for edges
    NamedRTree r;
    for (auto it = ec.begin(); it != ec.end(); ++it) {
        NBEdge* edge = it->second;
        Boundary bound = edge->getGeometry().getBoxBoundary();
        bound.grow(maxDist);
        float min[2] = { static_cast<float>(bound.xmin()), static_cast<float>(bound.ymin()) };
        float max[2] = { static_cast<float>(bound.xmax()), static_cast<float>(bound.ymax()) };
        r.Insert(min, max, edge);
    }

    for (auto itPoly = shc.getPolygons().begin(); itPoly != shc.getPolygons().end(); itPoly++) {
        SUMOPolygon* p = itPoly->second;
        Boundary bound = p->getShape().getBoxBoundary();
        float min[2] = { static_cast<float>(bound.xmin()), static_cast<float>(bound.ymin()) };
        float max[2] = { static_cast<float>(bound.xmax()), static_cast<float>(bound.ymax()) };
        std::set<const Named*> edges;
        Named::StoringVisitor visitor(edges);
        r.Search(min, max, visitor);
        std::vector<std::pair<double, std::string> > nearby;
        for (const Named* namedEdge : edges) {
            NBEdge* e = const_cast<NBEdge*>(dynamic_cast<const NBEdge*>(namedEdge));
            const double distance = VectorHelper<double>::minValue(p->getShape().distances(e->getLaneShape(0), true));
            if (distance <= maxDist) {
                // sort by distance and ID to stabilize results
                nearby.push_back(std::make_pair(distance, e->getID()));
                //std::cout << " poly=" << p->getID() << " e=" << e->getID() << " dist=" << distance << "\n";
            }
        }
        if (nearby.size() > 0) {
            std::sort(nearby.begin(), nearby.end());
            NBEdge* closest = ec.retrieve(nearby.front().second);
            std::string objects = closest->getParameter(ROAD_OBJECTS, "");
            if (objects != "") {
                objects += " ";
            }
            objects += p->getID();
            closest->setParameter(ROAD_OBJECTS, objects);
            //std::cout << "poly=" << p->getID() << " closest=" << closest->getID() << "\n";
        }
    }
    for (auto itPoi = shc.getPOIs().begin(); itPoi != shc.getPOIs().end(); itPoi++) {
        PointOfInterest* p = itPoi->second;
        float min[2] = { static_cast<float>(p->x()), static_cast<float>(p->y()) };
        float max[2] = { static_cast<float>(p->x()), static_cast<float>(p->y()) };
        std::set<const Named*> edges;
        Named::StoringVisitor visitor(edges);
        r.Search(min, max, visitor);
        std::vector<std::pair<double, std::string> > nearby;
        for (const Named* namedEdge : edges) {
            NBEdge* e = const_cast<NBEdge*>(dynamic_cast<const NBEdge*>(namedEdge));
            const double distance = e->getLaneShape(0).distance2D(*p, true);
            if (distance != GeomHelper::INVALID_OFFSET && distance <= maxDist) {
                // sort by distance and ID to stabilize results
                nearby.push_back(std::make_pair(distance, e->getID()));
                //if (p->getID() == "1275468911") {
                //    std::cout << " poly=" << p->getID() << " e=" << e->getID() << " dist=" << distance << "\n";
                //}
            }
        }
        if (nearby.size() > 0) {
            std::sort(nearby.begin(), nearby.end());
            NBEdge* closest = ec.retrieve(nearby.front().second);
            std::string objects = closest->getParameter(ROAD_OBJECTS, "");
            if (objects != "") {
                objects += " ";
            }
            objects += p->getID();
            closest->setParameter(ROAD_OBJECTS, objects);
            //std::cout << "poly=" << p->getID() << " closest=" << closest->getID() << "\n";
        }
    }
}

void
NWWriter_OpenDrive::writeSignal(OutputDevice& device, std::string id, double s, double t, std::string type, double hOffset) {
    device.openTag("signal");
    device.writeAttr("id", id);
    device.writeAttr("s", s);
    device.writeAttr("t", t);
    device.writeAttr("name", "signal:"+id);
    device.writeAttr("dynamic", "yes");
    device.writeAttr("orientation", "none");
    device.writeAttr("zOffset", 0);
    device.writeAttr("country", "OpenDRIVE");
    device.writeAttr("type", type); // "1000001" is traffic light
    device.writeAttr("subtype", "-1");
    device.writeAttr("value", -1);
    device.writeAttr("height", 0);
    device.writeAttr("width", 0.5);
    device.writeAttr("text", "");
    device.writeAttr("hOffset", hOffset);
    device.writeAttr("pitch", 0);
    device.writeAttr("roll", 0);
    device.openTag("validity");
    device.writeAttr("fromLane", 0);
    device.writeAttr("toLane", 0);
    device.closeTag();
    device.closeTag();
}

void
NWWriter_OpenDrive::writeSignalInertial(
        OutputDevice& device, std::string id, std::string type,
        double x, double y, double z,
        double hdg, double pitch, double roll) {
    device.openTag("signal");
    device.writeAttr("id", id);
    device.writeAttr("s", 0);
    device.writeAttr("t", 0);
    device.writeAttr("name", "signal:"+id);
    device.writeAttr("dynamic", "yes");
    device.writeAttr("orientation", "none");
    device.writeAttr("zOffset", 0);
    device.writeAttr("country", "OpenDRIVE");
    device.writeAttr("type", type); // "1000001" is traffic light
    device.writeAttr("subtype", "-1");
    device.writeAttr("value", -1);
    device.writeAttr("height", 0);
    device.writeAttr("width", 0.5);
    device.writeAttr("text", "");
    device.writeAttr("hOffset", 0);
    device.writeAttr("pitch", 0);
    device.writeAttr("roll", 0);
    device.openTag("validity");
    device.writeAttr("fromLane", 0);
    device.writeAttr("toLane", 0);
    device.closeTag();
    device.openTag("positionInertial");
    device.writeAttr("x", x);
    device.writeAttr("y", y);
    device.writeAttr("z", z);
    device.writeAttr("hdg", hdg);
    device.writeAttr("pitch", pitch);
    device.writeAttr("roll", roll);
    device.closeTag();
    device.closeTag();
}


void
NWWriter_OpenDrive::writeSignalReference(OutputDevice& device, std::string id, double s, double t, std::string orientation) {
    device.openTag("signalReference");
    device.writeAttr("id", id);
    device.writeAttr("s", s);
    device.writeAttr("t", t);
    device.writeAttr("orientation", orientation);
    device.closeTag();
}

std::pair<double, double>
NWWriter_OpenDrive::ComputePointSegmentDistance(
            const Position& P1, const Position& P2, const Position& P3) {
    Position P1P2 = P1 - P2;
    double DP1P2 = P1P2.dotProduct(P1P2);
    double t = (P3 - P1).dotProduct(P1P2) / DP1P2;
    double dist = P3.distanceTo(P1 - P1P2*t);
    return std::make_pair(t, dist);
}

void
NWWriter_OpenDrive::GenerateControllerRecord(OutputDevice& device, int controllerID, int signalID,int sequence){
    device.openTag("controller");
    device.writeAttr("name", "crtl" + std::to_string(controllerID));
    device.writeAttr("id", controllerID);
    device.writeAttr("sequence", sequence);
    device.openTag("control");
    device.writeAttr("signalId", signalID);
    device.writeAttr("type", "");
    device.closeTag();
    device.closeTag();
    controlled_signal_ids.insert(std::to_string(signalID));
}

void
NWWriter_OpenDrive::GenerateJunctionControllerRecord(OutputDevice& device, int controllerID, int sequence) {
    device.openTag("controller");
    device.writeAttr("id", controllerID);
    device.writeAttr("type", 0);
    device.writeAttr("sequence", sequence);
    device.closeTag();
}

void
NWWriter_OpenDrive::writeRoadObjectPOI(OutputDevice& device, const NBEdge* e, const PositionVector& roadShape, const PointOfInterest* poi) {
    Position center = *poi;
    const double edgeOffset = roadShape.nearest_offset_to_point2D(center, false);
    if (edgeOffset == GeomHelper::INVALID_OFFSET) {
        WRITE_WARNINGF("Cannot map road object POI '%' with center % onto edge '%'", poi->getID(), center, e->getID());
        return;
    }

    Position edgePos = roadShape.positionAtOffset2D(edgeOffset);
    double sideOffset = center.distanceTo2D(edgePos);
    // determine sign of sideOffset
    PositionVector tmp = roadShape.getSubpart2D(MAX2(0.0, edgeOffset - 1), MIN2(roadShape.length2D(), edgeOffset + 1));
    tmp.move2side(sideOffset);
    if (tmp.distance2D(center) < sideOffset) {
        sideOffset *= -1;
    }
    // place traffic signs on appropriate side of the road
    std::string type = poi->getShapeType();
    std::string name = StringUtils::escapeXML(poi->getParameter("name", ""), true);
    if (poi->getShapeType() == "traffic_sign") {
        sideOffset = getRoadSideOffset(e);
        type = "pole";
        name = "pole";
    }

    device.openTag("object");
    device.writeAttr("id", poi->getID());
    device.writeAttr("type", type);
    device.writeAttr("name", name);
    device.writeAttr("s", edgeOffset);
    device.writeAttr("t", sideOffset);
    device.writeAttr("hdg", 0);
    device.closeTag();
}

void
NWWriter_OpenDrive::writeRoadObjectPoly(OutputDevice& device, const NBEdge* e, const PositionVector& roadShape, const SUMOPolygon* p) {
    PositionVector shape = p->getShape();
    Position center = shape.getPolygonCenter();

    const double edgeOffset = roadShape.nearest_offset_to_point2D(center, false);
    if (edgeOffset == GeomHelper::INVALID_OFFSET) {
        WRITE_WARNINGF("Cannot map road object polygon '%' with center % onto edge '%'", p->getID(), center, e->getID());
        return;
    }
    Position edgePos = roadShape.positionAtOffset2D(edgeOffset);
    const double edgeAngle = roadShape.rotationAtOffset(edgeOffset);
    double sideOffset = center.distanceTo2D(edgePos);
    // determine sign of sideOffset
    PositionVector tmp = roadShape.getSubpart2D(MAX2(0.0, edgeOffset - 1), MIN2(roadShape.length2D(), edgeOffset + 1));
    tmp.move2side(sideOffset);
    if (tmp.distance2D(center) < sideOffset) {
        sideOffset *= -1;
    }
    //std::cout << " id=" << id
    //    << " shape=" << shape
    //    << " center=" << center
    //    << " edgeOffset=" << edgeOffset
    //    << "\n";
    auto shapeType = p->getShapeType();
    device.openTag("object");
    device.writeAttr("id", p->getID());
    device.writeAttr("type", shapeType);
    if (p->hasParameter("name")) {
        device.writeAttr("name", StringUtils::escapeXML(p->getParameter("name", ""), true));
    }
    device.writeAttr("s", edgeOffset);
    device.writeAttr("t", shapeType == "crosswalk" && !lefthand ? 0 : sideOffset);
    double hdg = -edgeAngle;
    if (p->hasParameter("hdg")) {
        try {
            hdg = StringUtils::toDoubleSecure(p->getParameter("hdg", ""), 0);
        } catch (NumberFormatException&) {}
    }
    device.writeAttr("hdg", hdg);
    if (p->hasParameter("length")) {
        try {
            device.writeAttr("length", StringUtils::toDoubleSecure(p->getParameter("length", ""), 0));
        } catch (NumberFormatException&) {}
    }
    if (p->hasParameter("width")) {
        try {
            device.writeAttr("width", StringUtils::toDoubleSecure(p->getParameter("width", ""), 0));
        } catch (NumberFormatException&) {}
    }
    double height = 0;
    if (p->hasParameter("height")) {
        try {
            height = StringUtils::toDoubleSecure(p->getParameter("height", ""), 0);
            device.writeAttr("height", height);
        } catch (NumberFormatException&) {}
    }
    //device.openTag("outlines");
    device.openTag("outline");
    device.writeAttr("id", 0);
    device.writeAttr("fillType", "concrete");
    device.writeAttr("outer", "true");
    device.writeAttr("closed", p->getShape().isClosed() ? "true" : "false");
    device.writeAttr("laneType", "border");

    shape.sub(center);
    if (hdg != -edgeAngle) {
        shape.rotate2D(-edgeAngle - hdg);
    }
    int i = 0;
    for (Position pos : shape) {
        device.openTag("cornerLocal");
        device.writeAttr("u", pos.x());
        device.writeAttr("v", pos.y());
        device.writeAttr("z", MAX2(0.0, p->getShapeLayer()));
        device.writeAttr("height", height);
        device.writeAttr("id", i++);
        device.closeTag();
    }


    //device.closeTag();
    device.closeTag();
    device.closeTag();
}

int
NWWriter_OpenDrive::getintID(std::string realid){
  if( id_redirections.find(realid) != id_redirections.end() ){
    return id_redirections[realid];
  }
  id_redirections[realid] = id_increment;
  ++id_increment;
  return id_redirections[realid];
}


/****************************************************************************/
