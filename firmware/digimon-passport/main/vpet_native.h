// main/vpet_native.h
// Native (non-emulated) Digital Monster Ver.5 prototype.
//
// The engine is a straight port of the logic recovered from the DoCoMo i-appli
// build (see research/v5-iappli).  It does not run the E0C6200 ROM: all data
// comes from the tables that were decoded out of t.sp.
#pragma once

#include "esp_err.h"

esp_err_t vpet_native_start(void);
