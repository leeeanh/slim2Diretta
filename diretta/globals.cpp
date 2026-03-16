/**
 * @file globals.cpp
 * @brief Global variable definitions for squeeze2diretta
 */

#include "globals.h"
#include "DirettaSync.h"

// Global log level - default INFO (same output as before)
LogLevel g_logLevel = LogLevel::INFO;

// Global verbose flag - kept for DirettaSync compatibility
bool g_verbose = false;

// Global SCHED_FIFO real-time priority for sender thread
int g_rtPriority = 50;

// Global SCHED_FIFO real-time priority for SDK worker thread (0 = auto: g_rtPriority+1)
int g_rtWorkerPriority = 0;

// Global CPU core affinity for RT threads (-1 = no pinning)
int g_rtCpuCore = -1;

// Global log ring for async logging in hot paths
// Allocated in main() if verbose mode is enabled
LogRing* g_logRing = nullptr;
