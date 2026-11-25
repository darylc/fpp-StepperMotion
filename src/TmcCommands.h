#pragma once
/*
 * FPP Commands, so the axes can be driven from playlists, the scheduler,
 * events and MQTT without spending a sequence channel on them.
 *
 * Every command here only ENQUEUES onto the owning axis worker; none of them
 * touch SPI. That keeps them serialised with show-driven motion exactly like
 * everything else.
 */

#include <string>
#include <vector>

class Command;

namespace tmc {

void registerTmcCommands();

// Withdraws AND deletes every command registered above: removeCommand()
// only unregisters, and a Command left registered after unload has its
// vtable in unmapped memory.
void unregisterTmcCommands();

} // namespace tmc
