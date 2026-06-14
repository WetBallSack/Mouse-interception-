#pragma once

#include <windows.h>

class KernelMouseRelay;

namespace SendInputDetour {

bool Install(KernelMouseRelay* relay);
void Uninstall();

}
