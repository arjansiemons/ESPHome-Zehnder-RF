"""Zehnder/Buva RF Ventilation Fan Component for ESPHome."""

import esphome.codegen as cg

# Component metadata
CODEOWNERS = ["@arjansiemons"]
DEPENDENCIES = ["nrf905"]
AUTO_LOAD = ["fan"]

# Namespace for C++ code
zehnder_ns = cg.esphome_ns.namespace("zehnder")
