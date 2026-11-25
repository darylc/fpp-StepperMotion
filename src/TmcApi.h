#pragma once
/*
 * HTTP routes backing the plugin's setup page.
 *
 * Registered through FPPPlugins::registerPluginApi() rather than straight on
 * drogon: drogon has no route-removal API, so a handler wired in directly
 * could never be withdrawn and the plugin could never be upgraded in place.
 */

#include <string>

namespace tmc {

// Internal drogon path.
extern const char* API_BASE_PATH;
// What the browser and FPP command argument lists must use.
extern const char* API_PUBLIC_PATH;

/*
 * The owner token is the plugin instance registering the routes. FPP can
 * hold a retired plugin object alive after its replacement has registered,
 * and the retired object's unregisterApis() would otherwise disarm the path
 * the live instance owns.
 */
void registerTmcApis(const void* owner);
void unregisterTmcApis(const void* owner);

} // namespace tmc
