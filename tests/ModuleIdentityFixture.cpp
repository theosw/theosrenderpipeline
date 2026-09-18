#include <Windows.h>

extern "C" __declspec(dllexport) int ModuleIdentity()
{
	return TRP_TEST_MODULE_ID;
}
