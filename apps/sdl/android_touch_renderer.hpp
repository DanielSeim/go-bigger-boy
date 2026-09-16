#pragma once

#ifdef __ANDROID__

namespace gbb::sdl {
class SdlResources;
void present_touch_controls(SdlResources& sdl);
}

#endif
