#pragma once

// Compile the real adapter's logging calls without an SDK logger dependency.
#define ESP_LOGE(...) ((void)0)
