// Standalone capture verifier and radio reader; shares Waffle's USB implementation.
// Build with CMake: cmake --build build --target cs7000-read
#include "../src/cs7000usb.h"

int main(int argc, char **argv)
{
    return Cs7000Usb::runCommandLine(argc, argv);
}
