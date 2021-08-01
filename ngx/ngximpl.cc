#include <stdint.h>
#include <string>
#include <map>

#include <stdio.h>

#define NGX_NO_FUNCS
#include "ngx.h"

struct NVSDK_NGX_Parameter_Impl: public NVSDK_NGX_Parameter {
	union u {
		void *p;
		long double d;
		uint64_t u64;

		u() : u64(0) {}
		u(void *p) : p(p) {}
		u(int i) : u64((int64_t)i) {}
		u(unsigned int ui) : u64(ui) {}
		u(float f) : d(f) {}
		u(long double d) : d(d) {}
		u(uint64_t u64) : u64(u64) {}
		operator void *() const { return p; }
		operator int() const { return (int)u64; }
		operator unsigned int() const { return u64; }
		operator float() const { return d; }
		operator long double() const { return d; }
		operator uint64_t() const { return u64; }
	};
	std::map<std::string, u> map;
	template<typename T> NVSDK_NGX_Result get(const char *name, T* p) {
		auto it = map.find(name);
		if (it != map.end()) {
			*p = it->second;
			return NVSDK_NGX_Result_Success;
		}
		return NVSDK_NGX_Result_Fail;
	}
	void Set(const char *, void *) override;
	void Set(const char *, struct ID3D12Resource *) override;
	void Set(const char *, struct ID3D11Resource *) override;
	void Set(const char *, int) override;
	void Set(const char *, unsigned int) override;
	void Set(const char *, long double) override;
	void Set(const char *, float) override;
	void Set(const char *, uint64_t) override;
	NVSDK_NGX_Result Get(const char *, void **) override;
	NVSDK_NGX_Result Get(const char *, struct ID3D12Resource **) override;
	NVSDK_NGX_Result Get(const char *, struct ID3D11Resource **) override;
	NVSDK_NGX_Result Get(const char *, int *) override;
	NVSDK_NGX_Result Get(const char *, unsigned int *) override;
	NVSDK_NGX_Result Get(const char *, long double *) override;
	NVSDK_NGX_Result Get(const char *, float *) override;
	NVSDK_NGX_Result Get(const char *, uint64_t *) override;
	void Reset() override;
};

void NVSDK_NGX_Parameter_Impl::Set(const char *name, void *p) { map.insert({ name, u(p) }); }
void NVSDK_NGX_Parameter_Impl::Set(const char *name, struct ID3D12Resource *p) { map.insert({ name, u((void*)p) }); }
void NVSDK_NGX_Parameter_Impl::Set(const char *name, struct ID3D11Resource *p) { map.insert({ name, u((void*)p) }); }
void NVSDK_NGX_Parameter_Impl::Set(const char *name, int i) { map.insert({ name, u(i) }); }
void NVSDK_NGX_Parameter_Impl::Set(const char *name, unsigned int ui) { map.insert({ name, u(ui) }); }
void NVSDK_NGX_Parameter_Impl::Set(const char *name, long double d) { map.insert({ name, u(d) }); }
void NVSDK_NGX_Parameter_Impl::Set(const char *name, float f) { map.insert({ name, u(f) }); }
void NVSDK_NGX_Parameter_Impl::Set(const char *name, uint64_t u64) { map.insert({ name, u(u64) }); }
NVSDK_NGX_Result NVSDK_NGX_Parameter_Impl::Get(const char *name, void **p) { return get<void*>(name, p); }
NVSDK_NGX_Result NVSDK_NGX_Parameter_Impl::Get(const char *name, struct ID3D12Resource **p) { return get<void*>(name, (void**)p); }
NVSDK_NGX_Result NVSDK_NGX_Parameter_Impl::Get(const char *name, struct ID3D11Resource **p) { return get<void*>(name, (void**)p); }
NVSDK_NGX_Result NVSDK_NGX_Parameter_Impl::Get(const char *name, int *p) { return get<int>(name, p); }
NVSDK_NGX_Result NVSDK_NGX_Parameter_Impl::Get(const char *name, unsigned int *p) { return get<unsigned int>(name, p); }
NVSDK_NGX_Result NVSDK_NGX_Parameter_Impl::Get(const char *name, long double *p) { return get<long double>(name, p); }
NVSDK_NGX_Result NVSDK_NGX_Parameter_Impl::Get(const char *name, float *p) { return get<float>(name, p); }
NVSDK_NGX_Result NVSDK_NGX_Parameter_Impl::Get(const char *name, uint64_t *p) { return get<uint64_t>(name, p); }
void NVSDK_NGX_Parameter_Impl::Reset() { map.clear(); }

void NV_new_Parameter(NVSDK_NGX_Parameter **p) {
	*p = new NVSDK_NGX_Parameter_Impl();
}
