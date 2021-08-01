#ifndef AUTODLL_H
#define AUTODLL_H

#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

template<typename T>
struct importDll {
#ifdef _WIN32
	importDll(T &x, const wchar_t *dllName, const char *displayname, std::vector<std::string> &errors, const char *name) {
		HMODULE h = LoadLibraryW(dllName);
#else
	importDll(T &x, const char *dllName, const char *name, std::vector<std::string> &errors) {
		const char *displayname = dllName;
		void *h = dlopen(dllName, RTLD_GLOBAL | RTLD_LAZY);
#endif
		if (h == 0) {
			errors.push_back(std::string("unable to load ") + displayname);
			return;
		}
#ifdef _WIN32
		x = reinterpret_cast<T>(GetProcAddress(h, name));
#else
		x = reinterpret_cast<T>(dlsym(h, name));
#endif
		if (x == nullptr) {
			errors.push_back(std::string("unable to find ") + name + " in " + displayname);
		}
	}
};

#define EXT_FN(dll, retty, name, args) \
	static retty (*name) args = nullptr; \
	static importDll<retty (*)args> _load ## name(name, dll, #name)

#endif
