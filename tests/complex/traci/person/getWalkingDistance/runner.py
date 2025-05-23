#!/usr/bin/env python
# -*- coding: utf-8 -*-
# Eclipse SUMO, Simulation of Urban MObility; see https://eclipse.dev/sumo
# Copyright (C) 2020-2025 German Aerospace Center (DLR) and others.
# This program and the accompanying materials are made available under the
# terms of the Eclipse Public License 2.0 which is available at
# https://www.eclipse.org/legal/epl-2.0/
# This Source Code may also be made available under the following Secondary
# Licenses when the conditions for such availability set forth in the Eclipse
# Public License 2.0 are satisfied: GNU General Public License, version 2
# or later which is available at
# https://www.gnu.org/licenses/old-licenses/gpl-2.0-standalone.html
# SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

# @file    runner.py
# @author  Jakob Erdmann
# @date    2024-09-23

import os
import sys

if "SUMO_HOME" in os.environ:
    sys.path.append(os.path.join(os.environ["SUMO_HOME"], "tools"))
import sumolib  # noqa
import traci  # noqa
import traci.constants as tc  # noqa

traci.start([sumolib.checkBinary("sumo"),
             "-n", "input_net3.net.xml",
             "--pedestrian.striping.dawdling", "0",
             "--no-step-log",
             ] + sys.argv[1:])
pID = "p1"
traci.person.add(pID, "SC", -10, traci.constants.DEPARTFLAG_NOW)
traci.person.appendWalkingStage(pID, ["SC", "CN"], arrivalPos=10)
try:
    print("getWalkingDistance", traci.person.getWalkingDistance(pID, "CN", 50))
except traci.TraCIException:
    pass
traci.simulationStep()
print("getWalkingDistance", traci.person.getWalkingDistance(pID, "CN", 50))
x, y = traci.simulation.convert2D("CN", 50)
print("getWalkingDistance2D", traci.person.getWalkingDistance2D(pID, x, y))

traci.close()
