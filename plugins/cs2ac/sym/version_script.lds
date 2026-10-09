{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* polyhook binding slots (defined by polyhook_bindings, filled by the plugify module
           from the polyhook plugin's exported methods) */
        __polyhook_*;

        /* cs2ac exports no Plugify methods: the Metamod-era IClientCvarValue001 interface had no
           consumer in the production plugin set, and every other interaction is one-directional
           (events/usercmds in, announcements/punishments out). */

    local: *;
};
