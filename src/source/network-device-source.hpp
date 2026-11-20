#pragma once

#include "core/device-types.hpp"

#include <obs-module.h>

void registerNetworkDeviceSource();
void applySourceConfigToData(obs_data_t *settings, const netdev::SourceConfig &cfg);
netdev::SourceConfig readSourceConfigFromData(obs_data_t *settings);
