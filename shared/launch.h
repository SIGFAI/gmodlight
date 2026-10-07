// Command line for the hidden Garry's Mod. Shared by the Dying Light plugin and tools/fakehost.
#pragma once
#include <cstdio>
#include <string>

namespace gml {

inline std::wstring GModCommandLine(const std::wstring& exe, unsigned w, unsigned h, const std::wstring& extra) {
    wchar_t cmd[2048];
    swprintf_s(cmd,
        // No -console: an open console pauses a singleplayer game.
        // -condebug: console output goes to garrysmod/console.log for troubleshooting.
        // -noworkshop: Workshop addons stay out of it (and out of its crashes). Not -noaddons:
        // that would also skip GModLight's own addon.
        L"\"%s\" -windowed -noborder -w %u -h %u -novid -nojoy -nohltv -condebug -noworkshop "
        // World, sky, fog and post-processing off: GMod's frame is just props, tools and UI
        // on a flat chroma key that Dying Light can see through. 4x MSAA: edges blend into
        // the key, which Dying Light un-mixes, so weapons get smooth edges over its scene.
        L"+sv_cheats 1 +r_drawworld 0 +r_drawstaticprops 0 +r_3dsky 0 +mat_antialias 4 "
        L"+mat_hdr_level 0 +mat_disable_bloom 1 +mat_colorcorrection 0 "
        L"+mat_motion_blur_enabled 0 +fog_override 1 +fog_enable 0 "
        // No entity interpolation: by default GMod draws moving things 100 ms in the past,
        // which left the physgun beam and held zombies trailing Dying Light.
        // 100 ticks a second (Source default 66): held zombies get updates more often.
        L"-tickrate 100 +sv_maxupdaterate 100 +sv_maxcmdrate 100 +cl_interp 0 +cl_interp_ratio 1 +cl_updaterate 100 +cl_cmdrate 100 "
        // fps_max is only a ceiling: GMod draws in lockstep with Dying Light (bridge.h
        // kFrameEventName), single-threaded so each frame is drawn and shown in order.
        L"+mat_queue_mode 0 +snd_mute_losefocus 0 +net_graph 0 +fps_max 200 +map gm_flatgrass %s",
        exe.c_str(), w, h, extra.c_str());
    return cmd;
}

}  // namespace gml
