{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* polyhook binding slots (defined by polyhook_bindings, filled by the plugify module
           from the polyhook plugin's exported methods) */
        __polyhook_*;

        /* admin_system binding slots (defined by the generated admin_system.cpp, filled by the
           plugify module from the admin_system plugin's exported methods) - the fork's
           admin_bridge queued bans use AddPlayerPunishment/AddOfflinePlayerPunishment */
        __admin_system_*;

        /* cs2ac exports no Plugify methods: the Metamod-era IClientCvarValue001 interface had no
           consumer among the addon's other plugins, and every other interaction is one-directional
           (events/usercmds in, announcements/punishments out). */

    local: *;
};
