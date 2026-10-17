#pragma once
// Exact EU/Asia guest audio helpers generated from the recorded source hashes.
static inline __attribute__((always_inline)) void r11_exact_sub_8289E638(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_8298F048(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_8289EC00(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_8289EB38(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_8298FF00(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_8289E538(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_8274CFE8(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_8289E3E0(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_8289E360(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_8289EAC0(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_8289E268(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_8289EA08(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_8289DED0(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_8289E058(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_828C3AA0(PPCContext& __restrict ctx, uint8_t* base);
static inline __attribute__((always_inline)) void r11_exact_sub_82168520(PPCContext& __restrict ctx, uint8_t* base);

static inline __attribute__((always_inline)) void r11_exact_sub_8289E638(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCRegister f30{};
	PPCRegister f31{};
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// stw r12,-8(r1)
	REX_STORE_U32(ctx.r1.u32 + -8, ctx.r12.u32);
	// stfd f30,-24(r1)
	ctx.fpscr.disableFlushMode();
	REX_STORE_U64(ctx.r1.u32 + -24, f30.u64);
	// stfd f31,-16(r1)
	REX_STORE_U64(ctx.r1.u32 + -16, f31.u64);
	// stwu r1,-112(r1)
	ea = -112 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// lfs f0,8(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 8);
	ctx.f0.f64 = double(temp.f32);
	// lis r11,-32245
	ctx.r11.s64 = -2113208320;
	// lfs f13,12(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 12);
	ctx.f13.f64 = double(temp.f32);
	// fsubs f12,f13,f0
	ctx.f12.f64 = double(float(ctx.f13.f64 - ctx.f0.f64));
	// lfs f11,20(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 20);
	ctx.f11.f64 = double(temp.f32);
	// lfs f31,4(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 4);
	f31.f64 = double(temp.f32);
	// lfs f30,16(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 16);
	f30.f64 = double(temp.f32);
	// fsel f13,f13,f12,f11
	ctx.f13.f64 = ctx.f13.f64 >= 0.0 ? ctx.f12.f64 : ctx.f11.f64;
	// fmadds f13,f13,f1,f0
	ctx.f13.f64 = double(float(std::fma(ctx.f13.f64, ctx.f1.f64, ctx.f0.f64)));
	// lfs f0,-6516(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -6516);
	ctx.f0.f64 = double(temp.f32);
	// fmuls f1,f13,f0
	ctx.f1.f64 = double(float(ctx.f13.f64 * ctx.f0.f64));
	// bl 0x8298f048
	ctx.lr = 0x8289E67C;
	r11_exact_sub_8298F048(ctx, base);
	// frsp f0,f1
	ctx.fpscr.disableFlushMode();
	ctx.f0.f64 = double(float(ctx.f1.f64));
	// fmadds f1,f0,f31,f30
	ctx.f1.f64 = double(float(std::fma(ctx.f0.f64, f31.f64, f30.f64)));
	// addi r1,r1,112
	ctx.r1.s64 = ctx.r1.s64 + 112;
	// lwz r12,-8(r1)
	ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 + -8);
	// mtlr r12
	ctx.lr = ctx.r12.u64;
	// lfd f30,-24(r1)
	f30.u64 = REX_LOAD_U64(ctx.r1.u32 + -24);
	// lfd f31,-16(r1)
	f31.u64 = REX_LOAD_U64(ctx.r1.u32 + -16);
	// blr 
	return;
}


static inline __attribute__((always_inline)) void r11_exact_sub_8298F048(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	// Keep leaf temporaries in host registers. Guest memory cannot alias PPCContext.
	PPCRegister r10_local_f0 = ctx.f0;
	PPCRegister r10_local_f1 = ctx.f1;
	PPCRegister r10_local_f8 = ctx.f8;
	PPCRegister r10_local_f9 = ctx.f9;
	PPCRegister r10_local_f10 = ctx.f10;
	PPCRegister r10_local_f11 = ctx.f11;
	PPCRegister r10_local_f12 = ctx.f12;
	PPCRegister r10_local_r1 = ctx.r1;
	PPCRegister r10_local_r10 = ctx.r10;
	PPCRegister r10_local_r11 = ctx.r11;
	auto r10_commit = [&]() {
		ctx.f0 = r10_local_f0;
		ctx.f1 = r10_local_f1;
		ctx.f8 = r10_local_f8;
		ctx.f9 = r10_local_f9;
		ctx.f10 = r10_local_f10;
		ctx.f11 = r10_local_f11;
		ctx.f12 = r10_local_f12;
		ctx.r1 = r10_local_r1;
		ctx.r10 = r10_local_r10;
		ctx.r11 = r10_local_r11;
	};

	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister temp{};
	// lis r11,-32087
	r10_local_r11.s64 = -2102853632;
	// fabs f12,f1
	ctx.fpscr.disableFlushMode();
	r10_local_f12.u64 = r10_local_f1.u64 & ~0x8000000000000000;
	// stfd f12,-8(r1)
	REX_STORE_U64(r10_local_r1.u32 + -8, r10_local_f12.u64);
	// addi r11,r11,26864
	r10_local_r11.s64 = r10_local_r11.s64 + 26864;
	// lfs f0,28(r11)
	temp.u32 = REX_LOAD_U32(r10_local_r11.u32 + 28);
	r10_local_f0.f64 = double(temp.f32);
	// lfs f9,32(r11)
	temp.u32 = REX_LOAD_U32(r10_local_r11.u32 + 32);
	r10_local_f9.f64 = double(temp.f32);
	// fsel f9,f1,f0,f9
	r10_local_f9.f64 = r10_local_f1.f64 >= 0.0 ? r10_local_f0.f64 : r10_local_f9.f64;
	// lfd f0,8(r11)
	r10_local_f0.u64 = REX_LOAD_U64(r10_local_r11.u32 + 8);
	// fmul f0,f0,f12
	r10_local_f0.f64 = r10_local_f0.f64 * r10_local_f12.f64;
	// lfd f13,40(r11)
	ctx.f13.u64 = REX_LOAD_U64(r10_local_r11.u32 + 40);
	// lfd f11,48(r11)
	r10_local_f11.u64 = REX_LOAD_U64(r10_local_r11.u32 + 48);
	// lfd f10,112(r11)
	r10_local_f10.u64 = REX_LOAD_U64(r10_local_r11.u32 + 112);
	// fctid f0,f0
	r10_local_f0.s64 = std::isnan(r10_local_f0.f64) ? int64_t(0x8000000000000000ULL) : (r10_local_f0.f64 > double(LLONG_MAX)) ? LLONG_MAX : simde_mm_cvtsd_si64(simde_mm_load_sd(&r10_local_f0.f64));
	// fcfid f0,f0
	r10_local_f0.f64 = double(r10_local_f0.s64);
	// fnmsub f13,f13,f0,f12
	ctx.f13.f64 = -std::fma(ctx.f13.f64, r10_local_f0.f64, -r10_local_f12.f64);
	// fctidz f8,f0
	r10_local_f8.s64 = std::isnan(r10_local_f0.f64) ? int64_t(0x8000000000000000ULL) : (r10_local_f0.f64 > double(LLONG_MAX)) ? LLONG_MAX : simde_mm_cvttsd_si64(simde_mm_load_sd(&r10_local_f0.f64));
	// stfd f8,-16(r1)
	REX_STORE_U64(r10_local_r1.u32 + -16, r10_local_f8.u64);
	// fnmsub f13,f11,f0,f13
	ctx.f13.f64 = -std::fma(r10_local_f11.f64, r10_local_f0.f64, -ctx.f13.f64);
	// lfd f11,104(r11)
	r10_local_f11.u64 = REX_LOAD_U64(r10_local_r11.u32 + 104);
	// fmul f0,f13,f13
	r10_local_f0.f64 = ctx.f13.f64 * ctx.f13.f64;
	// fmadd f10,f10,f0,f11
	r10_local_f10.f64 = std::fma(r10_local_f10.f64, r10_local_f0.f64, r10_local_f11.f64);
	// lfd f11,96(r11)
	r10_local_f11.u64 = REX_LOAD_U64(r10_local_r11.u32 + 96);
	// fmadd f10,f10,f0,f11
	r10_local_f10.f64 = std::fma(r10_local_f10.f64, r10_local_f0.f64, r10_local_f11.f64);
	// lfd f11,88(r11)
	r10_local_f11.u64 = REX_LOAD_U64(r10_local_r11.u32 + 88);
	// fmadd f10,f10,f0,f11
	r10_local_f10.f64 = std::fma(r10_local_f10.f64, r10_local_f0.f64, r10_local_f11.f64);
	// lfd f11,80(r11)
	r10_local_f11.u64 = REX_LOAD_U64(r10_local_r11.u32 + 80);
	// ld r10,-16(r1)
	r10_local_r10.u64 = REX_LOAD_U64(r10_local_r1.u32 + -16);
	// clrldi r10,r10,63
	r10_local_r10.u64 = r10_local_r10.u64 & 0x1;
	// cmpdi cr6,r10,0
	cr6.compare<int64_t>(r10_local_r10.s64, 0, xer);
	// lis r10,-32245
	r10_local_r10.s64 = -2113208320;
	// fmadd f10,f10,f0,f11
	r10_local_f10.f64 = std::fma(r10_local_f10.f64, r10_local_f0.f64, r10_local_f11.f64);
	// lfd f11,72(r11)
	r10_local_f11.u64 = REX_LOAD_U64(r10_local_r11.u32 + 72);
	// fmadd f10,f10,f0,f11
	r10_local_f10.f64 = std::fma(r10_local_f10.f64, r10_local_f0.f64, r10_local_f11.f64);
	// lfd f11,64(r11)
	r10_local_f11.u64 = REX_LOAD_U64(r10_local_r11.u32 + 64);
	// fmadd f10,f10,f0,f11
	r10_local_f10.f64 = std::fma(r10_local_f10.f64, r10_local_f0.f64, r10_local_f11.f64);
	// lfd f11,56(r11)
	r10_local_f11.u64 = REX_LOAD_U64(r10_local_r11.u32 + 56);
	// fmadd f10,f10,f0,f11
	r10_local_f10.f64 = std::fma(r10_local_f10.f64, r10_local_f0.f64, r10_local_f11.f64);
	// lfd f11,-6912(r10)
	r10_local_f11.u64 = REX_LOAD_U64(r10_local_r10.u32 + -6912);
	// fmadd f0,f10,f0,f11
	r10_local_f0.f64 = std::fma(r10_local_f10.f64, r10_local_f0.f64, r10_local_f11.f64);
	// fmul f0,f0,f13
	r10_local_f0.f64 = r10_local_f0.f64 * ctx.f13.f64;
	// beq cr6,0x8298f0f0
	if (cr6.eq) goto loc_8298F0F0;
	// fneg f0,f0
	r10_local_f0.u64 = r10_local_f0.u64 ^ 0x8000000000000000;
loc_8298F0F0:
	// ld r10,-8(r1)
	r10_local_r10.u64 = REX_LOAD_U64(r10_local_r1.u32 + -8);
	// fmul f13,f0,f9
	ctx.fpscr.disableFlushMode();
	ctx.f13.f64 = r10_local_f0.f64 * r10_local_f9.f64;
	// cmpdi cr6,r10,0
	cr6.compare<int64_t>(r10_local_r10.s64, 0, xer);
	// beqlr cr6
	if (cr6.eq) { r10_commit(); return; }
	// lfd f0,16(r11)
	r10_local_f0.u64 = REX_LOAD_U64(r10_local_r11.u32 + 16);
	// lis r11,-32087
	r10_local_r11.s64 = -2102853632;
	// fsub f12,f12,f0
	r10_local_f12.f64 = r10_local_f12.f64 - r10_local_f0.f64;
	// lfd f0,28648(r11)
	r10_local_f0.u64 = REX_LOAD_U64(r10_local_r11.u32 + 28648);
	// fsel f1,f12,f0,f13
	r10_local_f1.f64 = r10_local_f12.f64 >= 0.0 ? r10_local_f0.f64 : ctx.f13.f64;
	// blr 
	{ r10_commit(); return; }
}


static inline __attribute__((always_inline)) void r11_exact_sub_8289EC00(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCRegister ctr{};
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister temp{};
	// lwz r11,0(r3)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r3.u32 + 0);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(ctx.r11.u32, 0, xer);
	// beq cr6,0x8289ee38
	if (cr6.eq) goto loc_8289EE38;
	// lbz r11,36(r3)
	ctx.r11.u64 = REX_LOAD_U8(ctx.r3.u32 + 36);
	// cmplwi cr6,r11,4
	cr6.compare<uint32_t>(ctx.r11.u32, 4, xer);
	// bne cr6,0x8289ec1c
	if (!cr6.eq) goto loc_8289EC1C;
	// b 0x8289e058
	r11_exact_sub_8289E058(ctx, base);
	return;
loc_8289EC1C:
	// lbz r10,37(r3)
	ctx.r10.u64 = REX_LOAD_U8(ctx.r3.u32 + 37);
	// cmplwi cr6,r10,0
	cr6.compare<uint32_t>(ctx.r10.u32, 0, xer);
	// beq cr6,0x8289ec48
	if (cr6.eq) goto loc_8289EC48;
	// lfs f0,28(r3)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 28);
	ctx.f0.f64 = double(temp.f32);
	// fsubs f12,f1,f0
	ctx.f12.f64 = double(float(ctx.f1.f64 - ctx.f0.f64));
	// lfs f13,32(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 32);
	ctx.f13.f64 = double(temp.f32);
	// fsel f11,f12,f1,f0
	ctx.f11.f64 = ctx.f12.f64 >= 0.0 ? ctx.f1.f64 : ctx.f0.f64;
	// fsel f0,f12,f1,f0
	ctx.f0.f64 = ctx.f12.f64 >= 0.0 ? ctx.f1.f64 : ctx.f0.f64;
	// fsub f12,f11,f13
	ctx.f12.f64 = ctx.f11.f64 - ctx.f13.f64;
	// fsel f0,f12,f13,f0
	ctx.f0.f64 = ctx.f12.f64 >= 0.0 ? ctx.f13.f64 : ctx.f0.f64;
	// frsp f1,f0
	ctx.f1.f64 = double(float(ctx.f0.f64));
loc_8289EC48:
	// addi r11,r11,-1
	ctx.r11.s64 = ctx.r11.s64 + -1;
	// cmplwi cr6,r11,13
	cr6.compare<uint32_t>(ctx.r11.u32, 13, xer);
	// bgt cr6,0x8289ee38
	if (cr6.gt) goto loc_8289EE38;
	// lis r12,-32118
	ctx.r12.s64 = -2104885248;
	// addi r12,r12,-5012
	ctx.r12.s64 = ctx.r12.s64 + -5012;
	// rlwinm r0,r11,2,0,29
	ctx.r0.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 2) & 0xFFFFFFFC;
	// lwzx r0,r12,r0
	ctx.r0.u64 = REX_LOAD_U32(ctx.r12.u32 + ctx.r0.u32);
	// mtctr r0
	ctr.u64 = ctx.r0.u64;
	// bctr 
	switch (ctx.r11.u32) {
	case 0:
		goto loc_8289ECA4;
	case 1:
		goto loc_8289ECAC;
	case 2:
		goto loc_8289ECB0;
	case 3:
		goto loc_8289EE38;
	case 4:
		goto loc_8289ECB4;
	case 5:
		goto loc_8289ECB8;
	case 6:
		goto loc_8289ECBC;
	case 7:
		goto loc_8289ECC0;
	case 8:
		goto loc_8289ECC4;
	case 9:
		goto loc_8289ECC8;
	case 10:
		goto loc_8289ECCC;
	case 11:
		goto loc_8289ED08;
	case 12:
		goto loc_8289ED50;
	case 13:
		goto loc_8289EE34;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_8289ECA4:
	// lfs f1,4(r3)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 4);
	ctx.f1.f64 = double(temp.f32);
	// blr 
	return;
loc_8289ECAC:
	// b 0x8289ded0
	r11_exact_sub_8289DED0(ctx, base);
	return;
loc_8289ECB0:
	// b 0x8289ea08
	r11_exact_sub_8289EA08(ctx, base);
	return;
loc_8289ECB4:
	// b 0x8289e268
	r11_exact_sub_8289E268(ctx, base);
	return;
loc_8289ECB8:
	// b 0x8289eac0
	r11_exact_sub_8289EAC0(ctx, base);
	return;
loc_8289ECBC:
	// b 0x8289e360
	r11_exact_sub_8289E360(ctx, base);
	return;
loc_8289ECC0:
	// b 0x8289e3e0
	r11_exact_sub_8289E3E0(ctx, base);
	return;
loc_8289ECC4:
	// b 0x8289e538
	r11_exact_sub_8289E538(ctx, base);
	return;
loc_8289ECC8:
	// b 0x8289e638
	r11_exact_sub_8289E638(ctx, base);
	return;
loc_8289ECCC:
	// lfs f13,4(r3)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 4);
	ctx.f13.f64 = double(temp.f32);
	// lis r11,-32256
	ctx.r11.s64 = -2113929216;
	// fmuls f12,f13,f1
	ctx.f12.f64 = double(float(ctx.f13.f64 * ctx.f1.f64));
	// lfs f11,8(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 8);
	ctx.f11.f64 = double(temp.f32);
	// lfs f0,2592(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 2592);
	ctx.f0.f64 = double(temp.f32);
	// fadds f13,f13,f0
	ctx.f13.f64 = double(float(ctx.f13.f64 + ctx.f0.f64));
	// fadds f12,f12,f0
	ctx.f12.f64 = double(float(ctx.f12.f64 + ctx.f0.f64));
	// fdivs f13,f0,f13
	ctx.f13.f64 = double(float(ctx.f0.f64 / ctx.f13.f64));
	// fdivs f9,f0,f12
	ctx.f9.f64 = double(float(ctx.f0.f64 / ctx.f12.f64));
	// fsubs f10,f0,f13
	ctx.f10.f64 = double(float(ctx.f0.f64 - ctx.f13.f64));
	// fdivs f0,f0,f12
	ctx.f0.f64 = double(float(ctx.f0.f64 / ctx.f12.f64));
	// fsubs f13,f9,f13
	ctx.f13.f64 = double(float(ctx.f9.f64 - ctx.f13.f64));
	// fdivs f13,f13,f10
	ctx.f13.f64 = double(float(ctx.f13.f64 / ctx.f10.f64));
	// fsel f1,f11,f0,f13
	ctx.f1.f64 = ctx.f11.f64 >= 0.0 ? ctx.f0.f64 : ctx.f13.f64;
	// blr 
	return;
loc_8289ED08:
	// lis r11,-32256
	ctx.r11.s64 = -2113929216;
	// lfs f13,4(r3)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 4);
	ctx.f13.f64 = double(temp.f32);
	// fmuls f11,f13,f1
	ctx.f11.f64 = double(float(ctx.f13.f64 * ctx.f1.f64));
	// lfs f10,8(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 8);
	ctx.f10.f64 = double(temp.f32);
	// lfs f0,2592(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 2592);
	ctx.f0.f64 = double(temp.f32);
	// fadds f13,f13,f0
	ctx.f13.f64 = double(float(ctx.f13.f64 + ctx.f0.f64));
	// fdivs f12,f0,f13
	ctx.f12.f64 = double(float(ctx.f0.f64 / ctx.f13.f64));
	// fadds f13,f11,f0
	ctx.f13.f64 = double(float(ctx.f11.f64 + ctx.f0.f64));
	// fmuls f12,f12,f12
	ctx.f12.f64 = double(float(ctx.f12.f64 * ctx.f12.f64));
	// fmuls f9,f13,f13
	ctx.f9.f64 = double(float(ctx.f13.f64 * ctx.f13.f64));
	// fmuls f13,f13,f13
	ctx.f13.f64 = double(float(ctx.f13.f64 * ctx.f13.f64));
	// fsubs f11,f0,f12
	ctx.f11.f64 = double(float(ctx.f0.f64 - ctx.f12.f64));
	// fdivs f9,f0,f9
	ctx.f9.f64 = double(float(ctx.f0.f64 / ctx.f9.f64));
	// fdivs f0,f0,f13
	ctx.f0.f64 = double(float(ctx.f0.f64 / ctx.f13.f64));
	// fsubs f12,f9,f12
	ctx.f12.f64 = double(float(ctx.f9.f64 - ctx.f12.f64));
	// fdivs f12,f12,f11
	ctx.f12.f64 = double(float(ctx.f12.f64 / ctx.f11.f64));
	// fsel f1,f10,f0,f12
	ctx.f1.f64 = ctx.f10.f64 >= 0.0 ? ctx.f0.f64 : ctx.f12.f64;
	// blr 
	return;
loc_8289ED50:
	// lis r11,-32247
	ctx.r11.s64 = -2113339392;
	// lfs f0,-15288(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -15288);
	ctx.f0.f64 = double(temp.f32);
	// lis r11,-32247
	ctx.r11.s64 = -2113339392;
	// fsubs f13,f1,f0
	ctx.f13.f64 = double(float(ctx.f1.f64 - ctx.f0.f64));
	// lfs f12,-15292(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -15292);
	ctx.f12.f64 = double(temp.f32);
	// lis r11,-32247
	ctx.r11.s64 = -2113339392;
	// lfs f11,-15296(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -15296);
	ctx.f11.f64 = double(temp.f32);
	// lis r11,-32247
	ctx.r11.s64 = -2113339392;
	// lfs f10,-15300(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -15300);
	ctx.f10.f64 = double(temp.f32);
	// lis r11,-32247
	ctx.r11.s64 = -2113339392;
	// lfs f9,-15304(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -15304);
	ctx.f9.f64 = double(temp.f32);
	// lis r11,-32247
	ctx.r11.s64 = -2113339392;
	// lfs f8,-15308(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -15308);
	ctx.f8.f64 = double(temp.f32);
	// lis r11,-32247
	ctx.r11.s64 = -2113339392;
	// lfs f7,-15312(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -15312);
	ctx.f7.f64 = double(temp.f32);
	// lis r11,-32247
	ctx.r11.s64 = -2113339392;
	// lfs f6,-15316(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -15316);
	ctx.f6.f64 = double(temp.f32);
	// lis r11,-32247
	ctx.r11.s64 = -2113339392;
	// lfs f5,-15320(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -15320);
	ctx.f5.f64 = double(temp.f32);
	// lis r11,-32247
	ctx.r11.s64 = -2113339392;
	// lfs f0,-15324(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -15324);
	ctx.f0.f64 = double(temp.f32);
	// lis r11,-32247
	ctx.r11.s64 = -2113339392;
	// fsubs f3,f1,f0
	ctx.f3.f64 = double(float(ctx.f1.f64 - ctx.f0.f64));
	// lfs f0,-15328(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -15328);
	ctx.f0.f64 = double(temp.f32);
	// lis r11,-32245
	ctx.r11.s64 = -2113208320;
	// fmuls f0,f13,f0
	ctx.f0.f64 = double(float(ctx.f13.f64 * ctx.f0.f64));
	// lfs f13,-6976(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -6976);
	ctx.f13.f64 = double(temp.f32);
	// lis r11,-32247
	ctx.r11.s64 = -2113339392;
	// fsubs f2,f1,f13
	ctx.f2.f64 = double(float(ctx.f1.f64 - ctx.f13.f64));
	// lfs f4,-15332(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -15332);
	ctx.f4.f64 = double(temp.f32);
	// lis r11,-32256
	ctx.r11.s64 = -2113929216;
	// fmuls f13,f0,f0
	ctx.f13.f64 = double(float(ctx.f0.f64 * ctx.f0.f64));
	// fmsubs f12,f0,f12,f4
	ctx.f12.f64 = double(float(std::fma(ctx.f0.f64, ctx.f12.f64, -ctx.f4.f64)));
	// fmr f4,f13
	ctx.f4.f64 = ctx.f13.f64;
	// fmuls f13,f13,f0
	ctx.f13.f64 = double(float(ctx.f13.f64 * ctx.f0.f64));
	// fmadds f12,f4,f11,f12
	ctx.f12.f64 = double(float(std::fma(ctx.f4.f64, ctx.f11.f64, ctx.f12.f64)));
	// fmr f11,f13
	ctx.f11.f64 = ctx.f13.f64;
	// fmuls f13,f13,f0
	ctx.f13.f64 = double(float(ctx.f13.f64 * ctx.f0.f64));
	// fmadds f12,f11,f10,f12
	ctx.f12.f64 = double(float(std::fma(ctx.f11.f64, ctx.f10.f64, ctx.f12.f64)));
	// fmr f11,f13
	ctx.f11.f64 = ctx.f13.f64;
	// fmuls f13,f13,f0
	ctx.f13.f64 = double(float(ctx.f13.f64 * ctx.f0.f64));
	// fmadds f12,f11,f9,f12
	ctx.f12.f64 = double(float(std::fma(ctx.f11.f64, ctx.f9.f64, ctx.f12.f64)));
	// fmr f11,f13
	ctx.f11.f64 = ctx.f13.f64;
	// fmuls f13,f13,f0
	ctx.f13.f64 = double(float(ctx.f13.f64 * ctx.f0.f64));
	// fmadds f12,f11,f8,f12
	ctx.f12.f64 = double(float(std::fma(ctx.f11.f64, ctx.f8.f64, ctx.f12.f64)));
	// fmr f11,f13
	ctx.f11.f64 = ctx.f13.f64;
	// fmuls f13,f13,f0
	ctx.f13.f64 = double(float(ctx.f13.f64 * ctx.f0.f64));
	// fmadds f12,f11,f7,f12
	ctx.f12.f64 = double(float(std::fma(ctx.f11.f64, ctx.f7.f64, ctx.f12.f64)));
	// fmuls f11,f13,f0
	ctx.f11.f64 = double(float(ctx.f13.f64 * ctx.f0.f64));
	// fmadds f0,f13,f6,f12
	ctx.f0.f64 = double(float(std::fma(ctx.f13.f64, ctx.f6.f64, ctx.f12.f64)));
	// lfs f13,3068(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 3068);
	ctx.f13.f64 = double(temp.f32);
	// lis r11,-32247
	ctx.r11.s64 = -2113339392;
	// fmadds f0,f11,f5,f0
	ctx.f0.f64 = double(float(std::fma(ctx.f11.f64, ctx.f5.f64, ctx.f0.f64)));
	// fsel f0,f3,f0,f13
	ctx.f0.f64 = ctx.f3.f64 >= 0.0 ? ctx.f0.f64 : ctx.f13.f64;
	// lfs f13,-15648(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -15648);
	ctx.f13.f64 = double(temp.f32);
	// fsel f1,f2,f13,f0
	ctx.f1.f64 = ctx.f2.f64 >= 0.0 ? ctx.f13.f64 : ctx.f0.f64;
	// blr 
	return;
loc_8289EE34:
	// b 0x8289eb38
	r11_exact_sub_8289EB38(ctx, base);
	return;
loc_8289EE38:
	// lis r11,-32256
	ctx.r11.s64 = -2113929216;
	// lfs f1,3068(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 3068);
	ctx.f1.f64 = double(temp.f32);
	// blr 
	return;
}


static inline __attribute__((always_inline)) void r11_exact_sub_8289EB38(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r31{};
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// stw r12,-8(r1)
	REX_STORE_U32(ctx.r1.u32 + -8, ctx.r12.u32);
	// std r31,-16(r1)
	REX_STORE_U64(ctx.r1.u32 + -16, r31.u64);
	// stwu r1,-112(r1)
	ea = -112 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// lis r11,-32255
	ctx.r11.s64 = -2113863680;
	// lwz r31,0(r3)
	r31.u64 = REX_LOAD_U32(ctx.r3.u32 + 0);
	// lfs f0,29496(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 29496);
	ctx.f0.f64 = double(temp.f32);
	// fmuls f1,f1,f0
	ctx.f1.f64 = double(float(ctx.f1.f64 * ctx.f0.f64));
	// bl 0x8298ff00
	ctx.lr = 0x8289EB5C;
	r11_exact_sub_8298FF00(ctx, base);
	// frsp f0,f1
	ctx.fpscr.disableFlushMode();
	ctx.f0.f64 = double(float(ctx.f1.f64));
	// addi r9,r1,80
	ctx.r9.s64 = ctx.r1.s64 + 80;
	// lhz r11,21(r31)
	ctx.r11.u64 = REX_LOAD_U16(r31.u32 + 21);
	// addi r10,r11,-1
	ctx.r10.s64 = ctx.r11.s64 + -1;
	// fctiwz f0,f0
	ctx.f0.s64 = std::isnan(ctx.f0.f64) ? int64_t(0x80000000U) : (ctx.f0.f64 > double(INT_MAX)) ? INT_MAX : simde_mm_cvttsd_si32(simde_mm_load_sd(&ctx.f0.f64));
	// stfiwx f0,0,r9
	REX_STORE_U32(ctx.r9.u32, ctx.f0.u32);
	// lwz r11,80(r1)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 80);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(ctx.r11.s32, 0, xer);
	// bge cr6,0x8289eb88
	if (!cr6.lt) goto loc_8289EB88;
	// li r11,0
	ctx.r11.s64 = 0;
	// b 0x8289eb94
	goto loc_8289EB94;
loc_8289EB88:
	// cmpw cr6,r11,r10
	cr6.compare<int32_t>(ctx.r11.s32, ctx.r10.s32, xer);
	// ble cr6,0x8289eb94
	if (!cr6.gt) goto loc_8289EB94;
	// mr r11,r10
	ctx.r11.u64 = ctx.r10.u64;
loc_8289EB94:
	// rlwinm r11,r11,2,0,29
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 2) & 0xFFFFFFFC;
	// add r11,r11,r31
	ctx.r11.u64 = ctx.r11.u64 + r31.u64;
	// lwz r11,23(r11)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 23);
	// stw r11,80(r1)
	REX_STORE_U32(ctx.r1.u32 + 80, ctx.r11.u32);
	// lfs f1,80(r1)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 80);
	ctx.f1.f64 = double(temp.f32);
	// addi r1,r1,112
	ctx.r1.s64 = ctx.r1.s64 + 112;
	// lwz r12,-8(r1)
	ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 + -8);
	// mtlr r12
	ctx.lr = ctx.r12.u64;
	// ld r31,-16(r1)
	r31.u64 = REX_LOAD_U64(ctx.r1.u32 + -16);
	// blr 
	return;
}


static inline __attribute__((always_inline)) void r11_exact_sub_8298FF00(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	// fctidz f0,f1
	ctx.fpscr.disableFlushMode();
	ctx.f0.s64 = std::isnan(ctx.f1.f64) ? int64_t(0x8000000000000000ULL) : (ctx.f1.f64 > double(LLONG_MAX)) ? LLONG_MAX : simde_mm_cvttsd_si64(simde_mm_load_sd(&ctx.f1.f64));
	// lis r11,-32246
	ctx.r11.s64 = -2113273856;
	// fabs f13,f1
	ctx.f13.u64 = ctx.f1.u64 & ~0x8000000000000000;
	// lfd f12,-28824(r11)
	ctx.f12.u64 = REX_LOAD_U64(ctx.r11.u32 + -28824);
	// lis r11,-32245
	ctx.r11.s64 = -2113208320;
	// fcfid f0,f0
	ctx.f0.f64 = double(ctx.f0.s64);
	// fsub f12,f12,f13
	ctx.f12.f64 = ctx.f12.f64 - ctx.f13.f64;
	// fneg f11,f13
	ctx.f11.u64 = ctx.f13.u64 ^ 0x8000000000000000;
	// lfd f13,-6912(r11)
	ctx.f13.u64 = REX_LOAD_U64(ctx.r11.u32 + -6912);
	// fsub f10,f1,f0
	ctx.f10.f64 = ctx.f1.f64 - ctx.f0.f64;
	// fsub f13,f0,f13
	ctx.f13.f64 = ctx.f0.f64 - ctx.f13.f64;
	// fsel f0,f10,f0,f13
	ctx.f0.f64 = ctx.f10.f64 >= 0.0 ? ctx.f0.f64 : ctx.f13.f64;
	// fsel f0,f12,f0,f1
	ctx.f0.f64 = ctx.f12.f64 >= 0.0 ? ctx.f0.f64 : ctx.f1.f64;
	// fsel f1,f11,f1,f0
	ctx.f1.f64 = ctx.f11.f64 >= 0.0 ? ctx.f1.f64 : ctx.f0.f64;
	// blr 
	return;
}


static inline __attribute__((always_inline)) void r11_exact_sub_8289E538(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCRegister r31{};
	PPCRegister f29{};
	PPCRegister f30{};
	PPCRegister f31{};
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// stw r12,-8(r1)
	REX_STORE_U32(ctx.r1.u32 + -8, ctx.r12.u32);
	// std r31,-16(r1)
	REX_STORE_U64(ctx.r1.u32 + -16, r31.u64);
	// stfd f29,-40(r1)
	ctx.fpscr.disableFlushMode();
	REX_STORE_U64(ctx.r1.u32 + -40, f29.u64);
	// stfd f30,-32(r1)
	REX_STORE_U64(ctx.r1.u32 + -32, f30.u64);
	// stfd f31,-24(r1)
	REX_STORE_U64(ctx.r1.u32 + -24, f31.u64);
	// stwu r1,-160(r1)
	ea = -160 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// mr r31,r3
	r31.u64 = ctx.r3.u64;
	// lis r11,-32255
	ctx.r11.s64 = -2113863680;
	// addi r9,r1,80
	ctx.r9.s64 = ctx.r1.s64 + 80;
	// addi r10,r1,80
	ctx.r10.s64 = ctx.r1.s64 + 80;
	// lfs f0,4(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 4);
	ctx.f0.f64 = double(temp.f32);
	// fmuls f13,f0,f0
	ctx.f13.f64 = double(float(ctx.f0.f64 * ctx.f0.f64));
	// lfs f31,2756(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 2756);
	f31.f64 = double(temp.f32);
	// lis r11,-32245
	ctx.r11.s64 = -2113208320;
	// stfs f31,80(r1)
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 80, temp.u32);
	// fmuls f30,f0,f1
	f30.f64 = double(float(ctx.f0.f64 * ctx.f1.f64));
	// lvlx v1,0,r9
	temp.u32 = ctx.r9.u32;
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
	// lfs f29,-6944(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -6944);
	f29.f64 = double(temp.f32);
	// fmuls f0,f13,f29
	ctx.f0.f64 = double(float(ctx.f13.f64 * f29.f64));
	// stfs f0,80(r1)
	temp.f32 = float(ctx.f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 80, temp.u32);
	// lvlx v2,0,r10
	temp.u32 = ctx.r10.u32;
	simde_mm_store_si128((simde__m128i*)ctx.v2.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
	// bl 0x8274cfe8
	ctx.lr = 0x8289E594;
	r11_exact_sub_8274CFE8(ctx, base);
	// fmuls f0,f30,f30
	ctx.fpscr.disableFlushMode();
	ctx.f0.f64 = double(float(f30.f64 * f30.f64));
	// addi r9,r1,96
	ctx.r9.s64 = ctx.r1.s64 + 96;
	// addi r10,r1,80
	ctx.r10.s64 = ctx.r1.s64 + 80;
	// stfs f31,80(r1)
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 80, temp.u32);
	// addi r11,r1,80
	ctx.r11.s64 = ctx.r1.s64 + 80;
	// stvx128 v1,r0,r9
	ea = (ctx.r9.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v1.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// lvlx v1,0,r10
	temp.u32 = ctx.r10.u32;
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
	// fmuls f30,f0,f29
	f30.f64 = double(float(ctx.f0.f64 * f29.f64));
	// stfs f30,80(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 80, temp.u32);
	// lvlx v2,0,r11
	temp.u32 = ctx.r11.u32;
	simde_mm_store_si128((simde__m128i*)ctx.v2.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
	// bl 0x8274cfe8
	ctx.lr = 0x8289E5C0;
	r11_exact_sub_8274CFE8(ctx, base);
	// addi r9,r1,96
	ctx.r9.s64 = ctx.r1.s64 + 96;
	// lfs f13,96(r1)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 96);
	ctx.f13.f64 = double(temp.f32);
	// lis r11,-32256
	ctx.r11.s64 = -2113929216;
	// stfs f31,84(r1)
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 84, temp.u32);
	// addi r10,r1,80
	ctx.r10.s64 = ctx.r1.s64 + 80;
	// stfs f30,80(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 80, temp.u32);
	// stvx128 v1,r0,r9
	ea = (ctx.r9.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v1.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// lfs f0,2592(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 2592);
	ctx.f0.f64 = double(temp.f32);
	// addi r11,r1,84
	ctx.r11.s64 = ctx.r1.s64 + 84;
	// lfs f12,96(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 96);
	ctx.f12.f64 = double(temp.f32);
	// fsubs f0,f0,f13
	ctx.f0.f64 = double(float(ctx.f0.f64 - ctx.f13.f64));
	// fsubs f13,f12,f13
	ctx.f13.f64 = double(float(ctx.f12.f64 - ctx.f13.f64));
	// lvlx v2,0,r10
	temp.u32 = ctx.r10.u32;
	simde_mm_store_si128((simde__m128i*)ctx.v2.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
	// lvlx v1,0,r11
	temp.u32 = ctx.r11.u32;
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
	// fdivs f31,f13,f0
	f31.f64 = double(float(ctx.f13.f64 / ctx.f0.f64));
	// bl 0x8274cfe8
	ctx.lr = 0x8289E600;
	r11_exact_sub_8274CFE8(ctx, base);
	// addi r11,r1,96
	ctx.r11.s64 = ctx.r1.s64 + 96;
	// lfs f0,8(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + 8);
	ctx.f0.f64 = double(temp.f32);
	// stvx128 v1,r0,r11
	ea = (ctx.r11.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v1.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// lfs f13,96(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 96);
	ctx.f13.f64 = double(temp.f32);
	// fsel f1,f0,f13,f31
	ctx.f1.f64 = ctx.f0.f64 >= 0.0 ? ctx.f13.f64 : f31.f64;
	// addi r1,r1,160
	ctx.r1.s64 = ctx.r1.s64 + 160;
	// lwz r12,-8(r1)
	ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 + -8);
	// mtlr r12
	ctx.lr = ctx.r12.u64;
	// lfd f29,-40(r1)
	f29.u64 = REX_LOAD_U64(ctx.r1.u32 + -40);
	// lfd f30,-32(r1)
	f30.u64 = REX_LOAD_U64(ctx.r1.u32 + -32);
	// lfd f31,-24(r1)
	f31.u64 = REX_LOAD_U64(ctx.r1.u32 + -24);
	// ld r31,-16(r1)
	r31.u64 = REX_LOAD_U64(ctx.r1.u32 + -16);
	// blr 
	return;
}


static inline __attribute__((always_inline)) void r11_exact_sub_8274CFE8(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCRegister temp{};
	PPCVRegister vTemp{};
	// vrfiz v11,v2
	ctx.fpscr.enableFlushMode();
	simde_mm_store_ps(ctx.v11.f32, simde_mm_round_ps(simde_mm_load_ps(ctx.v2.f32), SIMDE_MM_FROUND_TO_ZERO | SIMDE_MM_FROUND_NO_EXC));
	// vspltisw v0,0
	simde_mm_store_si128((simde__m128i*)ctx.v0.u32, simde_mm_set1_epi32(int(0x0)));
	// vctsxs v9,v2,0
	simde_mm_store_si128((simde__m128i*)ctx.v9.s32, rex::ppc::simde_mm_vctsxs(simde_mm_load_ps(ctx.v2.f32)));
	// vspltisw v13,-1
	simde_mm_store_si128((simde__m128i*)ctx.v13.u32, simde_mm_set1_epi32(int(0xFFFFFFFF)));
	// vspltisw v12,1
	simde_mm_store_si128((simde__m128i*)ctx.v12.u32, simde_mm_set1_epi32(int(0x1)));
	// vspltisw v10,-9
	simde_mm_store_si128((simde__m128i*)ctx.v10.u32, simde_mm_set1_epi32(int(0xFFFFFFF7)));
	// vcmpgtfp v5,v0,v1
	simde_mm_store_ps(ctx.v5.f32, simde_mm_cmpgt_ps(simde_mm_load_ps(ctx.v0.f32), simde_mm_load_ps(ctx.v1.f32)));
	// vupkd3d128 v7,v0,4
	temp.f32 = 3.0f;
	temp.s32 += ctx.v0.s16[1];
	vTemp.f32[3] = temp.f32;
	temp.f32 = 3.0f;
	temp.s32 += ctx.v0.s16[0];
	vTemp.f32[2] = temp.f32;
	vTemp.f32[1] = 0.0f;
	vTemp.f32[0] = 1.0f;
	ctx.v7 = vTemp;
	// vcmpgtfp v6,v0,v2
	simde_mm_store_ps(ctx.v6.f32, simde_mm_cmpgt_ps(simde_mm_load_ps(ctx.v0.f32), simde_mm_load_ps(ctx.v2.f32)));
	// vslw v10,v13,v10
	{
		simde__m128i a = simde_mm_load_si128((simde__m128i*)ctx.v13.u8);
		simde__m128i b = simde_mm_load_si128((simde__m128i*)ctx.v10.u8);
		simde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_epi32(0x1F));
		simde_mm_store_si128((simde__m128i*)ctx.v10.u8, simde_mm_sllv_epi32(a, shift));
	}
	// vspltw v7,v7,3
	simde_mm_store_si128((simde__m128i*)ctx.v7.u32, simde_mm_shuffle_epi32(simde_mm_load_si128((simde__m128i*)ctx.v7.u32), 0x0));
	// vsrw v10,v10,v12
	ctx.v10.u32[0] = ctx.v10.u32[0] >> (ctx.v12.u8[0] & 0x1F);
	ctx.v10.u32[1] = ctx.v10.u32[1] >> (ctx.v12.u8[4] & 0x1F);
	ctx.v10.u32[2] = ctx.v10.u32[2] >> (ctx.v12.u8[8] & 0x1F);
	ctx.v10.u32[3] = ctx.v10.u32[3] >> (ctx.v12.u8[12] & 0x1F);
	// vcmpeqfp v8,v2,v11
	simde_mm_store_ps(ctx.v8.f32, simde_mm_cmpeq_ps(simde_mm_load_ps(ctx.v2.f32), simde_mm_load_ps(ctx.v11.f32)));
	// vslw v11,v13,v13
	{
		simde__m128i a = simde_mm_load_si128((simde__m128i*)ctx.v13.u8);
		simde__m128i b = simde_mm_load_si128((simde__m128i*)ctx.v13.u8);
		simde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_epi32(0x1F));
		simde_mm_store_si128((simde__m128i*)ctx.v11.u8, simde_mm_sllv_epi32(a, shift));
	}
	// vand v9,v9,v12
	simde_mm_store_si128((simde__m128i*)ctx.v9.u8, simde_mm_and_si128(simde_mm_load_si128((simde__m128i*)ctx.v9.u8), simde_mm_load_si128((simde__m128i*)ctx.v12.u8)));
	// vcmpeqfp v12,v2,v0
	simde_mm_store_ps(ctx.v12.f32, simde_mm_cmpeq_ps(simde_mm_load_ps(ctx.v2.f32), simde_mm_load_ps(ctx.v0.f32)));
	// vand v4,v1,v11
	simde_mm_store_si128((simde__m128i*)ctx.v4.u8, simde_mm_and_si128(simde_mm_load_si128((simde__m128i*)ctx.v1.u8), simde_mm_load_si128((simde__m128i*)ctx.v11.u8)));
	// vslw v9,v9,v13
	{
		simde__m128i a = simde_mm_load_si128((simde__m128i*)ctx.v9.u8);
		simde__m128i b = simde_mm_load_si128((simde__m128i*)ctx.v13.u8);
		simde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_epi32(0x1F));
		simde_mm_store_si128((simde__m128i*)ctx.v9.u8, simde_mm_sllv_epi32(a, shift));
	}
	// vcmpeqfp v13,v1,v0
	simde_mm_store_ps(ctx.v13.f32, simde_mm_cmpeq_ps(simde_mm_load_ps(ctx.v1.f32), simde_mm_load_ps(ctx.v0.f32)));
	// vandc v3,v1,v11
	simde_mm_store_si128((simde__m128i*)ctx.v3.u8, simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*)ctx.v11.u8), simde_mm_load_si128((simde__m128i*)ctx.v1.u8)));
	// vand v11,v4,v9
	simde_mm_store_si128((simde__m128i*)ctx.v11.u8, simde_mm_and_si128(simde_mm_load_si128((simde__m128i*)ctx.v4.u8), simde_mm_load_si128((simde__m128i*)ctx.v9.u8)));
	// vandc v9,v5,v8
	simde_mm_store_si128((simde__m128i*)ctx.v9.u8, simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*)ctx.v8.u8), simde_mm_load_si128((simde__m128i*)ctx.v5.u8)));
	// vlogefp v8,v3
	ctx.v8.f32[0] = log2f(ctx.v3.f32[0]);
	ctx.v8.f32[1] = log2f(ctx.v3.f32[1]);
	ctx.v8.f32[2] = log2f(ctx.v3.f32[2]);
	ctx.v8.f32[3] = log2f(ctx.v3.f32[3]);
	// vor v0,v0,v11
	simde_mm_store_si128((simde__m128i*)ctx.v0.u8, simde_mm_or_si128(simde_mm_load_si128((simde__m128i*)ctx.v0.u8), simde_mm_load_si128((simde__m128i*)ctx.v11.u8)));
	// vandc v6,v13,v6
	simde_mm_store_si128((simde__m128i*)ctx.v6.u8, simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*)ctx.v6.u8), simde_mm_load_si128((simde__m128i*)ctx.v13.u8)));
	// vor v13,v13,v12
	simde_mm_store_si128((simde__m128i*)ctx.v13.u8, simde_mm_or_si128(simde_mm_load_si128((simde__m128i*)ctx.v13.u8), simde_mm_load_si128((simde__m128i*)ctx.v12.u8)));
	// vsel v0,v10,v0,v6
	simde_mm_store_si128((simde__m128i*)ctx.v0.u8, simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*)ctx.v6.u8), simde_mm_load_si128((simde__m128i*)ctx.v10.u8)), simde_mm_and_si128(simde_mm_load_si128((simde__m128i*)ctx.v6.u8), simde_mm_load_si128((simde__m128i*)ctx.v0.u8))));
	// vor v13,v9,v13
	simde_mm_store_si128((simde__m128i*)ctx.v13.u8, simde_mm_or_si128(simde_mm_load_si128((simde__m128i*)ctx.v9.u8), simde_mm_load_si128((simde__m128i*)ctx.v13.u8)));
	// vmulfp128 v8,v8,v2
	simde_mm_store_ps(ctx.v8.f32, simde_mm_mul_ps(simde_mm_load_ps(ctx.v8.f32), simde_mm_load_ps(ctx.v2.f32)));
	// vsel v0,v0,v7,v12
	simde_mm_store_si128((simde__m128i*)ctx.v0.u8, simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*)ctx.v12.u8), simde_mm_load_si128((simde__m128i*)ctx.v0.u8)), simde_mm_and_si128(simde_mm_load_si128((simde__m128i*)ctx.v12.u8), simde_mm_load_si128((simde__m128i*)ctx.v7.u8))));
	// vexptefp v8,v8
	{
		simde__m128 x = simde_mm_load_ps(ctx.v8.f32);
		simde__m128 n = simde_mm_round_ps(x, SIMDE_MM_FROUND_TO_NEG_INF | SIMDE_MM_FROUND_NO_EXC);
		simde__m128 f = simde_mm_sub_ps(x, n);
		simde__m128 p = simde_mm_set1_ps(1.8775767e-3f);
		p = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(8.9893397e-3f));
		p = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(5.5826318e-2f));
		p = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(2.4015361e-1f));
		p = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(6.9315308e-1f));
		p = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(1.0f));
		simde__m128i exp_bits = simde_mm_slli_epi32(
			simde_mm_add_epi32(simde_mm_cvttps_epi32(n), simde_mm_set1_epi32(127)), 23);
		simde_mm_store_ps(ctx.v8.f32, simde_mm_mul_ps(p, simde_mm_castsi128_ps(exp_bits)));
	}
	// vor v11,v8,v11
	simde_mm_store_si128((simde__m128i*)ctx.v11.u8, simde_mm_or_si128(simde_mm_load_si128((simde__m128i*)ctx.v8.u8), simde_mm_load_si128((simde__m128i*)ctx.v11.u8)));
	// vsel v1,v11,v0,v13
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*)ctx.v13.u8), simde_mm_load_si128((simde__m128i*)ctx.v11.u8)), simde_mm_and_si128(simde_mm_load_si128((simde__m128i*)ctx.v13.u8), simde_mm_load_si128((simde__m128i*)ctx.v0.u8))));
	// blr 
	return;
}


static inline __attribute__((always_inline)) void r11_exact_sub_8289E3E0(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCRegister r31{};
	PPCRegister f29{};
	PPCRegister f30{};
	PPCRegister f31{};
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// stw r12,-8(r1)
	REX_STORE_U32(ctx.r1.u32 + -8, ctx.r12.u32);
	// std r31,-16(r1)
	REX_STORE_U64(ctx.r1.u32 + -16, r31.u64);
	// stfd f29,-40(r1)
	ctx.fpscr.disableFlushMode();
	REX_STORE_U64(ctx.r1.u32 + -40, f29.u64);
	// stfd f30,-32(r1)
	REX_STORE_U64(ctx.r1.u32 + -32, f30.u64);
	// stfd f31,-24(r1)
	REX_STORE_U64(ctx.r1.u32 + -24, f31.u64);
	// stwu r1,-160(r1)
	ea = -160 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// lis r11,-32255
	ctx.r11.s64 = -2113863680;
	// mr r31,r3
	r31.u64 = ctx.r3.u64;
	// addi r9,r1,80
	ctx.r9.s64 = ctx.r1.s64 + 80;
	// addi r10,r1,80
	ctx.r10.s64 = ctx.r1.s64 + 80;
	// lfs f31,2756(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 2756);
	f31.f64 = double(temp.f32);
	// lis r11,-32245
	ctx.r11.s64 = -2113208320;
	// lfs f0,4(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 4);
	ctx.f0.f64 = double(temp.f32);
	// stfs f31,80(r1)
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 80, temp.u32);
	// fmuls f29,f0,f1
	f29.f64 = double(float(ctx.f0.f64 * ctx.f1.f64));
	// lvlx v1,0,r9
	temp.u32 = ctx.r9.u32;
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
	// lfs f30,-6944(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + -6944);
	f30.f64 = double(temp.f32);
	// fmuls f13,f0,f30
	ctx.f13.f64 = double(float(ctx.f0.f64 * f30.f64));
	// stfs f13,80(r1)
	temp.f32 = float(ctx.f13.f64);
	REX_STORE_U32(ctx.r1.u32 + 80, temp.u32);
	// lvlx v2,0,r10
	temp.u32 = ctx.r10.u32;
	simde_mm_store_si128((simde__m128i*)ctx.v2.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
	// bl 0x8274cfe8
	ctx.lr = 0x8289E438;
	r11_exact_sub_8274CFE8(ctx, base);
	// addi r9,r1,96
	ctx.r9.s64 = ctx.r1.s64 + 96;
	// stfs f31,80(r1)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 80, temp.u32);
	// addi r10,r1,80
	ctx.r10.s64 = ctx.r1.s64 + 80;
	// fmuls f30,f29,f30
	f30.f64 = double(float(f29.f64 * f30.f64));
	// addi r11,r1,80
	ctx.r11.s64 = ctx.r1.s64 + 80;
	// stvx128 v1,r0,r9
	ea = (ctx.r9.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v1.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// lvlx v1,0,r10
	temp.u32 = ctx.r10.u32;
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
	// stfs f30,80(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 80, temp.u32);
	// lvlx v2,0,r11
	temp.u32 = ctx.r11.u32;
	simde_mm_store_si128((simde__m128i*)ctx.v2.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
	// bl 0x8274cfe8
	ctx.lr = 0x8289E460;
	r11_exact_sub_8274CFE8(ctx, base);
	// addi r9,r1,96
	ctx.r9.s64 = ctx.r1.s64 + 96;
	// lfs f13,96(r1)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 96);
	ctx.f13.f64 = double(temp.f32);
	// lis r11,-32256
	ctx.r11.s64 = -2113929216;
	// stfs f31,84(r1)
	temp.f32 = float(f31.f64);
	REX_STORE_U32(ctx.r1.u32 + 84, temp.u32);
	// addi r10,r1,80
	ctx.r10.s64 = ctx.r1.s64 + 80;
	// stfs f30,80(r1)
	temp.f32 = float(f30.f64);
	REX_STORE_U32(ctx.r1.u32 + 80, temp.u32);
	// stvx128 v1,r0,r9
	ea = (ctx.r9.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v1.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// lfs f0,2592(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 2592);
	ctx.f0.f64 = double(temp.f32);
	// addi r11,r1,84
	ctx.r11.s64 = ctx.r1.s64 + 84;
	// lfs f12,96(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 96);
	ctx.f12.f64 = double(temp.f32);
	// fsubs f0,f0,f13
	ctx.f0.f64 = double(float(ctx.f0.f64 - ctx.f13.f64));
	// fsubs f13,f12,f13
	ctx.f13.f64 = double(float(ctx.f12.f64 - ctx.f13.f64));
	// lvlx v2,0,r10
	temp.u32 = ctx.r10.u32;
	simde_mm_store_si128((simde__m128i*)ctx.v2.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
	// lvlx v1,0,r11
	temp.u32 = ctx.r11.u32;
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
	// fdivs f31,f13,f0
	f31.f64 = double(float(ctx.f13.f64 / ctx.f0.f64));
	// bl 0x8274cfe8
	ctx.lr = 0x8289E4A0;
	r11_exact_sub_8274CFE8(ctx, base);
	// addi r11,r1,96
	ctx.r11.s64 = ctx.r1.s64 + 96;
	// lfs f0,8(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + 8);
	ctx.f0.f64 = double(temp.f32);
	// stvx128 v1,r0,r11
	ea = (ctx.r11.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v1.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// lfs f13,96(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 96);
	ctx.f13.f64 = double(temp.f32);
	// fsel f1,f0,f13,f31
	ctx.f1.f64 = ctx.f0.f64 >= 0.0 ? ctx.f13.f64 : f31.f64;
	// addi r1,r1,160
	ctx.r1.s64 = ctx.r1.s64 + 160;
	// lwz r12,-8(r1)
	ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 + -8);
	// mtlr r12
	ctx.lr = ctx.r12.u64;
	// lfd f29,-40(r1)
	f29.u64 = REX_LOAD_U64(ctx.r1.u32 + -40);
	// lfd f30,-32(r1)
	f30.u64 = REX_LOAD_U64(ctx.r1.u32 + -32);
	// lfd f31,-24(r1)
	f31.u64 = REX_LOAD_U64(ctx.r1.u32 + -24);
	// ld r31,-16(r1)
	r31.u64 = REX_LOAD_U64(ctx.r1.u32 + -16);
	// blr 
	return;
}


static inline __attribute__((always_inline)) void r11_exact_sub_8289E360(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCRegister r31{};
	PPCRegister f31{};
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// stw r12,-8(r1)
	REX_STORE_U32(ctx.r1.u32 + -8, ctx.r12.u32);
	// std r31,-16(r1)
	REX_STORE_U64(ctx.r1.u32 + -16, r31.u64);
	// stfd f31,-24(r1)
	ctx.fpscr.disableFlushMode();
	REX_STORE_U64(ctx.r1.u32 + -24, f31.u64);
	// stwu r1,-144(r1)
	ea = -144 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// lis r11,-32256
	ctx.r11.s64 = -2113929216;
	// mr r31,r3
	r31.u64 = ctx.r3.u64;
	// addi r10,r1,80
	ctx.r10.s64 = ctx.r1.s64 + 80;
	// lfs f31,2592(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 2592);
	f31.f64 = double(temp.f32);
	// addi r11,r1,80
	ctx.r11.s64 = ctx.r1.s64 + 80;
	// fsubs f0,f31,f1
	ctx.f0.f64 = double(float(f31.f64 - ctx.f1.f64));
	// lfs f13,4(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 4);
	ctx.f13.f64 = double(temp.f32);
	// lfs f12,12(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 12);
	ctx.f12.f64 = double(temp.f32);
	// stfs f12,80(r1)
	temp.f32 = float(ctx.f12.f64);
	REX_STORE_U32(ctx.r1.u32 + 80, temp.u32);
	// lvlx v2,0,r11
	temp.u32 = ctx.r11.u32;
	simde_mm_store_si128((simde__m128i*)ctx.v2.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
	// fsel f0,f13,f1,f0
	ctx.f0.f64 = ctx.f13.f64 >= 0.0 ? ctx.f1.f64 : ctx.f0.f64;
	// stfs f0,80(r1)
	temp.f32 = float(ctx.f0.f64);
	REX_STORE_U32(ctx.r1.u32 + 80, temp.u32);
	// lvlx v1,0,r10
	temp.u32 = ctx.r10.u32;
	simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
	// bl 0x8274cfe8
	ctx.lr = 0x8289E3AC;
	r11_exact_sub_8274CFE8(ctx, base);
	// addi r11,r1,96
	ctx.r11.s64 = ctx.r1.s64 + 96;
	// lfs f12,8(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + 8);
	ctx.f12.f64 = double(temp.f32);
	// stvx128 v1,r0,r11
	ea = (ctx.r11.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v1.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// lfs f0,96(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 96);
	ctx.f0.f64 = double(temp.f32);
	// fsubs f13,f31,f0
	ctx.f13.f64 = double(float(f31.f64 - ctx.f0.f64));
	// fsel f1,f12,f0,f13
	ctx.f1.f64 = ctx.f12.f64 >= 0.0 ? ctx.f0.f64 : ctx.f13.f64;
	// addi r1,r1,144
	ctx.r1.s64 = ctx.r1.s64 + 144;
	// lwz r12,-8(r1)
	ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 + -8);
	// mtlr r12
	ctx.lr = ctx.r12.u64;
	// lfd f31,-24(r1)
	f31.u64 = REX_LOAD_U64(ctx.r1.u32 + -24);
	// ld r31,-16(r1)
	r31.u64 = REX_LOAD_U64(ctx.r1.u32 + -16);
	// blr 
	return;
}


static inline __attribute__((always_inline)) void r11_exact_sub_8289EAC0(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r31{};
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// stw r12,-8(r1)
	REX_STORE_U32(ctx.r1.u32 + -8, ctx.r12.u32);
	// std r31,-16(r1)
	REX_STORE_U64(ctx.r1.u32 + -16, r31.u64);
	// stwu r1,-112(r1)
	ea = -112 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// lwz r31,0(r3)
	r31.u64 = REX_LOAD_U32(ctx.r3.u32 + 0);
	// bl 0x8298ff00
	ctx.lr = 0x8289EAD8;
	r11_exact_sub_8298FF00(ctx, base);
	// frsp f0,f1
	ctx.fpscr.disableFlushMode();
	ctx.f0.f64 = double(float(ctx.f1.f64));
	// addi r9,r1,80
	ctx.r9.s64 = ctx.r1.s64 + 80;
	// lhz r11,21(r31)
	ctx.r11.u64 = REX_LOAD_U16(r31.u32 + 21);
	// addi r10,r11,-1
	ctx.r10.s64 = ctx.r11.s64 + -1;
	// fctiwz f0,f0
	ctx.f0.s64 = std::isnan(ctx.f0.f64) ? int64_t(0x80000000U) : (ctx.f0.f64 > double(INT_MAX)) ? INT_MAX : simde_mm_cvttsd_si32(simde_mm_load_sd(&ctx.f0.f64));
	// stfiwx f0,0,r9
	REX_STORE_U32(ctx.r9.u32, ctx.f0.u32);
	// lwz r11,80(r1)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 80);
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(ctx.r11.s32, 0, xer);
	// bge cr6,0x8289eb04
	if (!cr6.lt) goto loc_8289EB04;
	// li r11,0
	ctx.r11.s64 = 0;
	// b 0x8289eb10
	goto loc_8289EB10;
loc_8289EB04:
	// cmpw cr6,r11,r10
	cr6.compare<int32_t>(ctx.r11.s32, ctx.r10.s32, xer);
	// ble cr6,0x8289eb10
	if (!cr6.gt) goto loc_8289EB10;
	// mr r11,r10
	ctx.r11.u64 = ctx.r10.u64;
loc_8289EB10:
	// rlwinm r11,r11,2,0,29
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 2) & 0xFFFFFFFC;
	// add r11,r11,r31
	ctx.r11.u64 = ctx.r11.u64 + r31.u64;
	// lwz r11,23(r11)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 23);
	// stw r11,80(r1)
	REX_STORE_U32(ctx.r1.u32 + 80, ctx.r11.u32);
	// lfs f1,80(r1)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + 80);
	ctx.f1.f64 = double(temp.f32);
	// addi r1,r1,112
	ctx.r1.s64 = ctx.r1.s64 + 112;
	// lwz r12,-8(r1)
	ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 + -8);
	// mtlr r12
	ctx.lr = ctx.r12.u64;
	// ld r31,-16(r1)
	r31.u64 = REX_LOAD_U64(ctx.r1.u32 + -16);
	// blr 
	return;
}


static inline __attribute__((always_inline)) void r11_exact_sub_8289E268(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCRegister r31{};
	PPCRegister f31{};
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// stw r12,-8(r1)
	REX_STORE_U32(ctx.r1.u32 + -8, ctx.r12.u32);
	// std r31,-16(r1)
	REX_STORE_U64(ctx.r1.u32 + -16, r31.u64);
	// stfd f31,-24(r1)
	ctx.fpscr.disableFlushMode();
	REX_STORE_U64(ctx.r1.u32 + -24, f31.u64);
	// stwu r1,-112(r1)
	ea = -112 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// lis r11,-32256
	ctx.r11.s64 = -2113929216;
	// mr r31,r3
	r31.u64 = ctx.r3.u64;
	// lfs f31,2592(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 2592);
	f31.f64 = double(temp.f32);
	// lis r11,-32245
	ctx.r11.s64 = -2113208320;
	// fsubs f0,f31,f1
	ctx.f0.f64 = double(float(f31.f64 - ctx.f1.f64));
	// lfs f13,4(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 4);
	ctx.f13.f64 = double(temp.f32);
	// fsel f0,f13,f1,f0
	ctx.f0.f64 = ctx.f13.f64 >= 0.0 ? ctx.f1.f64 : ctx.f0.f64;
	// lfd f13,-6920(r11)
	ctx.f13.u64 = REX_LOAD_U64(ctx.r11.u32 + -6920);
	// lis r11,-32245
	ctx.r11.s64 = -2113208320;
	// fsel f12,f0,f0,f13
	ctx.f12.f64 = ctx.f0.f64 >= 0.0 ? ctx.f0.f64 : ctx.f13.f64;
	// fsel f13,f0,f0,f13
	ctx.f13.f64 = ctx.f0.f64 >= 0.0 ? ctx.f0.f64 : ctx.f13.f64;
	// lfd f0,-6912(r11)
	ctx.f0.u64 = REX_LOAD_U64(ctx.r11.u32 + -6912);
	// lis r11,-32255
	ctx.r11.s64 = -2113863680;
	// fsub f12,f12,f0
	ctx.f12.f64 = ctx.f12.f64 - ctx.f0.f64;
	// fsel f0,f12,f0,f13
	ctx.f0.f64 = ctx.f12.f64 >= 0.0 ? ctx.f0.f64 : ctx.f13.f64;
	// frsp f13,f0
	ctx.f13.f64 = double(float(ctx.f0.f64));
	// lfs f0,18916(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 18916);
	ctx.f0.f64 = double(temp.f32);
	// fmuls f1,f13,f0
	ctx.f1.f64 = double(float(ctx.f13.f64 * ctx.f0.f64));
	// bl 0x8298f048
	ctx.lr = 0x8289E2C8;
	r11_exact_sub_8298F048(ctx, base);
	// frsp f0,f1
	ctx.fpscr.disableFlushMode();
	ctx.f0.f64 = double(float(ctx.f1.f64));
	// lfs f12,8(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 8);
	ctx.f12.f64 = double(temp.f32);
	// fsubs f13,f31,f0
	ctx.f13.f64 = double(float(f31.f64 - ctx.f0.f64));
	// fsel f1,f12,f0,f13
	ctx.f1.f64 = ctx.f12.f64 >= 0.0 ? ctx.f0.f64 : ctx.f13.f64;
	// addi r1,r1,112
	ctx.r1.s64 = ctx.r1.s64 + 112;
	// lwz r12,-8(r1)
	ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 + -8);
	// mtlr r12
	ctx.lr = ctx.r12.u64;
	// lfd f31,-24(r1)
	f31.u64 = REX_LOAD_U64(ctx.r1.u32 + -24);
	// ld r31,-16(r1)
	r31.u64 = REX_LOAD_U64(ctx.r1.u32 + -16);
	// blr 
	return;
}


static inline __attribute__((always_inline)) void r11_exact_sub_8289EA08(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCCRRegister cr6{};
	PPCRegister temp{};
	uint32_t ea{};
	// lfs f0,4(r3)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 4);
	ctx.f0.f64 = double(temp.f32);
	// fcmpu cr6,f1,f0
	cr6.compare(ctx.f1.f64, ctx.f0.f64);
	// bgt cr6,0x8289ea1c
	if (cr6.gt) goto loc_8289EA1C;
	// lfs f0,8(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 8);
	ctx.f0.f64 = double(temp.f32);
	// b 0x8289ea5c
	goto loc_8289EA5C;
loc_8289EA1C:
	// lfs f11,12(r3)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 12);
	ctx.f11.f64 = double(temp.f32);
	// fcmpu cr6,f1,f11
	cr6.compare(ctx.f1.f64, ctx.f11.f64);
	// blt cr6,0x8289ea30
	if (cr6.lt) goto loc_8289EA30;
	// lfs f0,16(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 16);
	ctx.f0.f64 = double(temp.f32);
	// b 0x8289ea5c
	goto loc_8289EA5C;
loc_8289EA30:
	// lfs f0,8(r3)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 8);
	ctx.f0.f64 = double(temp.f32);
	// lfs f12,16(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 16);
	ctx.f12.f64 = double(temp.f32);
	// fcmpu cr6,f0,f12
	cr6.compare(ctx.f0.f64, ctx.f12.f64);
	// beq cr6,0x8289ea5c
	if (cr6.eq) goto loc_8289EA5C;
	// lfs f0,4(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 4);
	ctx.f0.f64 = double(temp.f32);
	// fsubs f10,f1,f0
	ctx.f10.f64 = double(float(ctx.f1.f64 - ctx.f0.f64));
	// lfs f13,8(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 8);
	ctx.f13.f64 = double(temp.f32);
	// fsubs f0,f11,f0
	ctx.f0.f64 = double(float(ctx.f11.f64 - ctx.f0.f64));
	// fsubs f12,f12,f13
	ctx.f12.f64 = double(float(ctx.f12.f64 - ctx.f13.f64));
	// fdivs f0,f10,f0
	ctx.f0.f64 = double(float(ctx.f10.f64 / ctx.f0.f64));
	// fmadds f0,f0,f12,f13
	ctx.f0.f64 = double(float(std::fma(ctx.f0.f64, ctx.f12.f64, ctx.f13.f64)));
loc_8289EA5C:
	// lis r11,-32247
	ctx.r11.s64 = -2113339392;
	// stfs f0,-16(r1)
	ctx.fpscr.disableFlushMode();
	temp.f32 = float(ctx.f0.f64);
	REX_STORE_U32(ctx.r1.u32 + -16, temp.u32);
	// li r10,16
	ctx.r10.s64 = 16;
	// addi r11,r11,-15648
	ctx.r11.s64 = ctx.r11.s64 + -15648;
	// lvx128 v0,r11,r10
	ea = (ctx.r11.u32 + ctx.r10.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)ctx.v0.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// lis r10,-32256
	ctx.r10.s64 = -2113929216;
	// lfs f13,3068(r10)
	temp.u32 = REX_LOAD_U32(ctx.r10.u32 + 3068);
	ctx.f13.f64 = double(temp.f32);
	// stfs f13,-12(r1)
	temp.f32 = float(ctx.f13.f64);
	REX_STORE_U32(ctx.r1.u32 + -12, temp.u32);
	// stfs f13,-8(r1)
	temp.f32 = float(ctx.f13.f64);
	REX_STORE_U32(ctx.r1.u32 + -8, temp.u32);
	// addi r10,r1,-16
	ctx.r10.s64 = ctx.r1.s64 + -16;
	// lfs f13,4(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 4);
	ctx.f13.f64 = double(temp.f32);
	// fsubs f12,f0,f13
	ctx.f12.f64 = double(float(ctx.f0.f64 - ctx.f13.f64));
	// lvx128 v13,r0,r10
	ea = (ctx.r10.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)ctx.v13.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// addi r10,r1,-16
	ctx.r10.s64 = ctx.r1.s64 + -16;
	// vlogefp v13,v13
	ctx.fpscr.enableFlushModeUnconditional();
	ctx.v13.f32[0] = log2f(ctx.v13.f32[0]);
	ctx.v13.f32[1] = log2f(ctx.v13.f32[1]);
	ctx.v13.f32[2] = log2f(ctx.v13.f32[2]);
	ctx.v13.f32[3] = log2f(ctx.v13.f32[3]);
	// vmulfp128 v0,v13,v0
	simde_mm_store_ps(ctx.v0.f32, simde_mm_mul_ps(simde_mm_load_ps(ctx.v13.f32), simde_mm_load_ps(ctx.v0.f32)));
	// stvx128 v0,r0,r10
	ea = (ctx.r10.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)ctx.v0.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));
	// lis r10,-32256
	ctx.r10.s64 = -2113929216;
	// lfs f13,-16(r1)
	ctx.fpscr.disableFlushModeUnconditional();
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + -16);
	ctx.f13.f64 = double(temp.f32);
	// lfs f0,5248(r10)
	temp.u32 = REX_LOAD_U32(ctx.r10.u32 + 5248);
	ctx.f0.f64 = double(temp.f32);
	// fmuls f0,f13,f0
	ctx.f0.f64 = double(float(ctx.f13.f64 * ctx.f0.f64));
	// lfs f13,0(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 0);
	ctx.f13.f64 = double(temp.f32);
	// fsel f1,f12,f0,f13
	ctx.f1.f64 = ctx.f12.f64 >= 0.0 ? ctx.f0.f64 : ctx.f13.f64;
	// blr 
	return;
}


static inline __attribute__((always_inline)) void r11_exact_sub_8289DED0(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCCRRegister cr6{};
	PPCRegister temp{};
	// fmr f10,f1
	ctx.fpscr.disableFlushMode();
	ctx.f10.f64 = ctx.f1.f64;
	// lfs f0,4(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 4);
	ctx.f0.f64 = double(temp.f32);
	// fcmpu cr6,f10,f0
	cr6.compare(ctx.f10.f64, ctx.f0.f64);
	// bgt cr6,0x8289dee8
	if (cr6.gt) goto loc_8289DEE8;
	// lfs f1,8(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 8);
	ctx.f1.f64 = double(temp.f32);
	// blr 
	return;
loc_8289DEE8:
	// lfs f11,12(r3)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 12);
	ctx.f11.f64 = double(temp.f32);
	// fcmpu cr6,f10,f11
	cr6.compare(ctx.f10.f64, ctx.f11.f64);
	// blt cr6,0x8289defc
	if (cr6.lt) goto loc_8289DEFC;
	// lfs f1,16(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 16);
	ctx.f1.f64 = double(temp.f32);
	// blr 
	return;
loc_8289DEFC:
	// lfs f1,8(r3)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 8);
	ctx.f1.f64 = double(temp.f32);
	// lfs f12,16(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 16);
	ctx.f12.f64 = double(temp.f32);
	// fcmpu cr6,f1,f12
	cr6.compare(ctx.f1.f64, ctx.f12.f64);
	// beqlr cr6
	if (cr6.eq) return;
	// lfs f0,4(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 4);
	ctx.f0.f64 = double(temp.f32);
	// fmr f13,f1
	ctx.f13.f64 = ctx.f1.f64;
	// fsubs f10,f10,f0
	ctx.f10.f64 = double(float(ctx.f10.f64 - ctx.f0.f64));
	// fsubs f0,f11,f0
	ctx.f0.f64 = double(float(ctx.f11.f64 - ctx.f0.f64));
	// fsubs f12,f12,f13
	ctx.f12.f64 = double(float(ctx.f12.f64 - ctx.f13.f64));
	// fdivs f0,f10,f0
	ctx.f0.f64 = double(float(ctx.f10.f64 / ctx.f0.f64));
	// fmadds f1,f0,f12,f13
	ctx.f1.f64 = double(float(std::fma(ctx.f0.f64, ctx.f12.f64, ctx.f13.f64)));
	// blr 
	return;
}


static inline __attribute__((always_inline)) void r11_exact_sub_8289E058(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister temp{};
	// lbz r11,38(r3)
	ctx.r11.u64 = REX_LOAD_U8(ctx.r3.u32 + 38);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(ctx.r11.u32, 0, xer);
	// beq cr6,0x8289e0b8
	if (cr6.eq) goto loc_8289E0B8;
	// lfs f13,4(r3)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 4);
	ctx.f13.f64 = double(temp.f32);
	// fsubs f12,f1,f13
	ctx.f12.f64 = double(float(ctx.f1.f64 - ctx.f13.f64));
	// lfs f0,16(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 16);
	ctx.f0.f64 = double(temp.f32);
	// lfs f13,8(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 8);
	ctx.f13.f64 = double(temp.f32);
	// lfs f9,28(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 28);
	ctx.f9.f64 = double(temp.f32);
	// fsubs f6,f0,f13
	ctx.f6.f64 = double(float(ctx.f0.f64 - ctx.f13.f64));
	// lfs f10,12(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 12);
	ctx.f10.f64 = double(temp.f32);
	// fsubs f10,f1,f10
	ctx.f10.f64 = double(float(ctx.f1.f64 - ctx.f10.f64));
	// lfs f11,24(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 24);
	ctx.f11.f64 = double(temp.f32);
	// lfs f8,32(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 32);
	ctx.f8.f64 = double(temp.f32);
	// fsubs f5,f11,f0
	ctx.f5.f64 = double(float(ctx.f11.f64 - ctx.f0.f64));
	// lfs f7,20(r3)
	temp.u32 = REX_LOAD_U32(ctx.r3.u32 + 20);
	ctx.f7.f64 = double(temp.f32);
	// fsubs f7,f1,f7
	ctx.f7.f64 = double(float(ctx.f1.f64 - ctx.f7.f64));
	// fmuls f9,f9,f12
	ctx.f9.f64 = double(float(ctx.f9.f64 * ctx.f12.f64));
	// fmuls f8,f8,f10
	ctx.f8.f64 = double(float(ctx.f8.f64 * ctx.f10.f64));
	// fmadds f9,f9,f6,f13
	ctx.f9.f64 = double(float(std::fma(ctx.f9.f64, ctx.f6.f64, ctx.f13.f64)));
	// fmadds f0,f8,f5,f0
	ctx.f0.f64 = double(float(std::fma(ctx.f8.f64, ctx.f5.f64, ctx.f0.f64)));
	// fsel f13,f12,f9,f13
	ctx.f13.f64 = ctx.f12.f64 >= 0.0 ? ctx.f9.f64 : ctx.f13.f64;
	// fsel f0,f10,f0,f13
	ctx.f0.f64 = ctx.f10.f64 >= 0.0 ? ctx.f0.f64 : ctx.f13.f64;
	// fsel f1,f7,f11,f0
	ctx.f1.f64 = ctx.f7.f64 >= 0.0 ? ctx.f11.f64 : ctx.f0.f64;
	// blr 
	return;
loc_8289E0B8:
	// lwz r10,0(r3)
	ctx.r10.u64 = REX_LOAD_U32(ctx.r3.u32 + 0);
	// li r11,1
	ctx.r11.s64 = 1;
	// addi r8,r10,25
	ctx.r8.s64 = ctx.r10.s64 + 25;
	// lwz r9,21(r10)
	ctx.r9.u64 = REX_LOAD_U32(ctx.r10.u32 + 21);
	// rlwinm r10,r9,3,0,28
	ctx.r10.u64 = __builtin_rotateleft64(ctx.r9.u32 | (ctx.r9.u64 << 32), 3) & 0xFFFFFFF8;
	// lwz r7,0(r8)
	ctx.r7.u64 = REX_LOAD_U32(ctx.r8.u32 + 0);
	// addi r6,r9,-1
	ctx.r6.s64 = ctx.r9.s64 + -1;
	// add r10,r10,r8
	ctx.r10.u64 = ctx.r10.u64 + ctx.r8.u64;
	// cmpwi cr6,r6,4
	cr6.compare<int32_t>(ctx.r6.s32, 4, xer);
	// stw r7,-16(r1)
	REX_STORE_U32(ctx.r1.u32 + -16, ctx.r7.u32);
	// lwz r10,-8(r10)
	ctx.r10.u64 = REX_LOAD_U32(ctx.r10.u32 + -8);
	// stw r10,-12(r1)
	REX_STORE_U32(ctx.r1.u32 + -12, ctx.r10.u32);
	// lfs f0,-16(r1)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + -16);
	ctx.f0.f64 = double(temp.f32);
	// lfs f13,-12(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + -12);
	ctx.f13.f64 = double(temp.f32);
	// fsubs f12,f1,f13
	ctx.f12.f64 = double(float(ctx.f1.f64 - ctx.f13.f64));
	// fsel f13,f12,f13,f1
	ctx.f13.f64 = ctx.f12.f64 >= 0.0 ? ctx.f13.f64 : ctx.f1.f64;
	// fsubs f12,f0,f13
	ctx.f12.f64 = double(float(ctx.f0.f64 - ctx.f13.f64));
	// fsel f0,f12,f0,f13
	ctx.f0.f64 = ctx.f12.f64 >= 0.0 ? ctx.f0.f64 : ctx.f13.f64;
	// blt cr6,0x8289e16c
	if (cr6.lt) goto loc_8289E16C;
	// addi r10,r8,24
	ctx.r10.s64 = ctx.r8.s64 + 24;
loc_8289E108:
	// lwz r7,-16(r10)
	ctx.r7.u64 = REX_LOAD_U32(ctx.r10.u32 + -16);
	// stw r7,-12(r1)
	REX_STORE_U32(ctx.r1.u32 + -12, ctx.r7.u32);
	// lfs f13,-12(r1)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + -12);
	ctx.f13.f64 = double(temp.f32);
	// fcmpu cr6,f13,f0
	cr6.compare(ctx.f13.f64, ctx.f0.f64);
	// bge cr6,0x8289e1b8
	if (!cr6.lt) goto loc_8289E1B8;
	// lwz r7,-8(r10)
	ctx.r7.u64 = REX_LOAD_U32(ctx.r10.u32 + -8);
	// stw r7,-16(r1)
	REX_STORE_U32(ctx.r1.u32 + -16, ctx.r7.u32);
	// lfs f13,-16(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + -16);
	ctx.f13.f64 = double(temp.f32);
	// fcmpu cr6,f13,f0
	cr6.compare(ctx.f13.f64, ctx.f0.f64);
	// bge cr6,0x8289e1a4
	if (!cr6.lt) goto loc_8289E1A4;
	// lwz r7,0(r10)
	ctx.r7.u64 = REX_LOAD_U32(ctx.r10.u32 + 0);
	// stw r7,-8(r1)
	REX_STORE_U32(ctx.r1.u32 + -8, ctx.r7.u32);
	// lfs f13,-8(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + -8);
	ctx.f13.f64 = double(temp.f32);
	// fcmpu cr6,f13,f0
	cr6.compare(ctx.f13.f64, ctx.f0.f64);
	// bge cr6,0x8289e1ac
	if (!cr6.lt) goto loc_8289E1AC;
	// lwz r7,8(r10)
	ctx.r7.u64 = REX_LOAD_U32(ctx.r10.u32 + 8);
	// stw r7,-4(r1)
	REX_STORE_U32(ctx.r1.u32 + -4, ctx.r7.u32);
	// lfs f13,-4(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + -4);
	ctx.f13.f64 = double(temp.f32);
	// fcmpu cr6,f13,f0
	cr6.compare(ctx.f13.f64, ctx.f0.f64);
	// bge cr6,0x8289e1b4
	if (!cr6.lt) goto loc_8289E1B4;
	// addi r11,r11,4
	ctx.r11.s64 = ctx.r11.s64 + 4;
	// addi r7,r9,-3
	ctx.r7.s64 = ctx.r9.s64 + -3;
	// addi r10,r10,32
	ctx.r10.s64 = ctx.r10.s64 + 32;
	// cmplw cr6,r11,r7
	cr6.compare<uint32_t>(ctx.r11.u32, ctx.r7.u32, xer);
	// blt cr6,0x8289e108
	if (cr6.lt) goto loc_8289E108;
loc_8289E16C:
	// cmplw cr6,r11,r9
	cr6.compare<uint32_t>(ctx.r11.u32, ctx.r9.u32, xer);
	// bge cr6,0x8289e1b8
	if (!cr6.lt) goto loc_8289E1B8;
	// rlwinm r10,r11,3,0,28
	ctx.r10.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 3) & 0xFFFFFFF8;
	// add r10,r10,r8
	ctx.r10.u64 = ctx.r10.u64 + ctx.r8.u64;
loc_8289E17C:
	// lwz r7,0(r10)
	ctx.r7.u64 = REX_LOAD_U32(ctx.r10.u32 + 0);
	// stw r7,-4(r1)
	REX_STORE_U32(ctx.r1.u32 + -4, ctx.r7.u32);
	// lfs f13,-4(r1)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + -4);
	ctx.f13.f64 = double(temp.f32);
	// fcmpu cr6,f13,f0
	cr6.compare(ctx.f13.f64, ctx.f0.f64);
	// bge cr6,0x8289e1b8
	if (!cr6.lt) goto loc_8289E1B8;
	// addi r11,r11,1
	ctx.r11.s64 = ctx.r11.s64 + 1;
	// addi r10,r10,8
	ctx.r10.s64 = ctx.r10.s64 + 8;
	// cmplw cr6,r11,r9
	cr6.compare<uint32_t>(ctx.r11.u32, ctx.r9.u32, xer);
	// blt cr6,0x8289e17c
	if (cr6.lt) goto loc_8289E17C;
	// b 0x8289e1b8
	goto loc_8289E1B8;
loc_8289E1A4:
	// addi r11,r11,1
	ctx.r11.s64 = ctx.r11.s64 + 1;
	// b 0x8289e1b8
	goto loc_8289E1B8;
loc_8289E1AC:
	// addi r11,r11,2
	ctx.r11.s64 = ctx.r11.s64 + 2;
	// b 0x8289e1b8
	goto loc_8289E1B8;
loc_8289E1B4:
	// addi r11,r11,3
	ctx.r11.s64 = ctx.r11.s64 + 3;
loc_8289E1B8:
	// rlwinm r11,r11,3,0,28
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 3) & 0xFFFFFFF8;
	// add r11,r11,r8
	ctx.r11.u64 = ctx.r11.u64 + ctx.r8.u64;
	// lwz r10,-8(r11)
	ctx.r10.u64 = REX_LOAD_U32(ctx.r11.u32 + -8);
	// lwz r8,0(r11)
	ctx.r8.u64 = REX_LOAD_U32(ctx.r11.u32 + 0);
	// lwz r9,-4(r11)
	ctx.r9.u64 = REX_LOAD_U32(ctx.r11.u32 + -4);
	// lwz r11,4(r11)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 4);
	// stw r10,-4(r1)
	REX_STORE_U32(ctx.r1.u32 + -4, ctx.r10.u32);
	// stw r8,-12(r1)
	REX_STORE_U32(ctx.r1.u32 + -12, ctx.r8.u32);
	// stw r9,-8(r1)
	REX_STORE_U32(ctx.r1.u32 + -8, ctx.r9.u32);
	// stw r11,-16(r1)
	REX_STORE_U32(ctx.r1.u32 + -16, ctx.r11.u32);
	// lfs f13,-4(r1)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + -4);
	ctx.f13.f64 = double(temp.f32);
	// lfs f11,-12(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + -12);
	ctx.f11.f64 = double(temp.f32);
	// fsubs f0,f0,f13
	ctx.f0.f64 = double(float(ctx.f0.f64 - ctx.f13.f64));
	// fsubs f13,f11,f13
	ctx.f13.f64 = double(float(ctx.f11.f64 - ctx.f13.f64));
	// lfs f12,-8(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + -8);
	ctx.f12.f64 = double(temp.f32);
	// lfs f10,-16(r1)
	temp.u32 = REX_LOAD_U32(ctx.r1.u32 + -16);
	ctx.f10.f64 = double(temp.f32);
	// fsubs f11,f10,f12
	ctx.f11.f64 = double(float(ctx.f10.f64 - ctx.f12.f64));
	// fdivs f0,f0,f13
	ctx.f0.f64 = double(float(ctx.f0.f64 / ctx.f13.f64));
	// fmadds f1,f0,f11,f12
	ctx.f1.f64 = double(float(std::fma(ctx.f0.f64, ctx.f11.f64, ctx.f12.f64)));
	// blr 
	return;
}


static inline __attribute__((always_inline)) void r11_exact_sub_828C3AA0(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	// The PPC context is private to this guest thread. Keep temporaries local,
	// committing every context field at calls and exits, reloading after calls.
	PPCRegister r11_local_f0 = ctx.f0;
	PPCRegister r11_local_f1 = ctx.f1;
	PPCRegister r11_local_f11 = ctx.f11;
	PPCRegister r11_local_f12 = ctx.f12;
	PPCRegister r11_local_r1 = ctx.r1;
	PPCRegister r11_local_r3 = ctx.r3;
	PPCRegister r11_local_r4 = ctx.r4;
	PPCRegister r11_local_r7 = ctx.r7;
	PPCRegister r11_local_r8 = ctx.r8;
	PPCRegister r11_local_r9 = ctx.r9;
	PPCRegister r11_local_r10 = ctx.r10;
	PPCRegister r11_local_r11 = ctx.r11;
	PPCRegister r11_local_r12 = ctx.r12;
	auto r11_commit = [&]() {
		ctx.f0 = r11_local_f0;
		ctx.f1 = r11_local_f1;
		ctx.f11 = r11_local_f11;
		ctx.f12 = r11_local_f12;
		ctx.r1 = r11_local_r1;
		ctx.r3 = r11_local_r3;
		ctx.r4 = r11_local_r4;
		ctx.r7 = r11_local_r7;
		ctx.r8 = r11_local_r8;
		ctx.r9 = r11_local_r9;
		ctx.r10 = r11_local_r10;
		ctx.r11 = r11_local_r11;
		ctx.r12 = r11_local_r12;
	};
	auto r11_reload = [&]() {
		r11_local_f0 = ctx.f0;
		r11_local_f1 = ctx.f1;
		r11_local_f11 = ctx.f11;
		r11_local_f12 = ctx.f12;
		r11_local_r1 = ctx.r1;
		r11_local_r3 = ctx.r3;
		r11_local_r4 = ctx.r4;
		r11_local_r7 = ctx.r7;
		r11_local_r8 = ctx.r8;
		r11_local_r9 = ctx.r9;
		r11_local_r10 = ctx.r10;
		r11_local_r11 = ctx.r11;
		r11_local_r12 = ctx.r12;
	};

	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r26{};
	PPCRegister r27{};
	PPCRegister r28{};
	PPCRegister r29{};
	PPCRegister r30{};
	PPCRegister r31{};
	PPCRegister f26{};
	PPCRegister f27{};
	PPCRegister f28{};
	PPCRegister f29{};
	PPCRegister f30{};
	PPCRegister f31{};
	PPCRegister temp{};
	uint32_t ea{};
	uint8_t* r11_sample_host = nullptr;
	bool r11_contiguous_samples = false;
	// mflr r12
	r11_local_r12.u64 = ctx.lr;
	// bl 0x8298eac0
	ctx.lr = 0x828C3AA8;
	// addi r12,r1,-56
	r11_local_r12.s64 = r11_local_r1.s64 + -56;
	// bl 0x829900f0
	ctx.lr = 0x828C3AB0;
	// stwu r1,-224(r1)
	ea = -224 + r11_local_r1.u32;
	REX_STORE_U32(ea, r11_local_r1.u32);
	r11_local_r1.u32 = ea;
	// mr r31,r3
	r31.u64 = r11_local_r3.u64;
	// mr r3,r4
	r11_local_r3.u64 = r11_local_r4.u64;
	// lbz r11,29(r31)
	r11_local_r11.u64 = REX_LOAD_U8(r31.u32 + 29);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11_local_r11.u32, 0, xer);
	// bne cr6,0x828c3c0c
	if (!cr6.eq) goto loc_828C3C0C;
	// addi r4,r1,96
	r11_local_r4.s64 = r11_local_r1.s64 + 96;
	// bl 0x82168520
	ctx.lr = 0x828C3AD0;
	r11_commit();
	r11_exact_sub_82168520(ctx, base);
	r11_reload();
	// lbz r11,97(r1)
	r11_local_r11.u64 = REX_LOAD_U8(r11_local_r1.u32 + 97);
	// li r27,0
	r27.s64 = 0;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11_local_r11.u32, 0, xer);
	// beq cr6,0x828c3c0c
	if (cr6.eq) goto loc_828C3C0C;
	// lis r7,-32256
	r11_local_r7.s64 = -2113929216;
	// lis r8,-32245
	r11_local_r8.s64 = -2113208320;
	// lis r9,-32256
	r11_local_r9.s64 = -2113929216;
	// lis r10,-32245
	r11_local_r10.s64 = -2113208320;
	// lis r11,-32256
	r11_local_r11.s64 = -2113929216;
	// lfs f26,9928(r7)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r11_local_r7.u32 + 9928);
	f26.f64 = double(temp.f32);
	// li r28,0
	r28.s64 = 0;
	// lfd f29,-6912(r8)
	f29.u64 = REX_LOAD_U64(r11_local_r8.u32 + -6912);
	// li r26,1
	r26.s64 = 1;
	// lfd f30,9920(r9)
	f30.u64 = REX_LOAD_U64(r11_local_r9.u32 + 9920);
	// lfs f27,-6944(r10)
	temp.u32 = REX_LOAD_U32(r11_local_r10.u32 + -6944);
	f27.f64 = double(temp.f32);
	// lfs f28,2592(r11)
	temp.u32 = REX_LOAD_U32(r11_local_r11.u32 + 2592);
	f28.f64 = double(temp.f32);
loc_828C3B10:
	// lbz r11,46(r31)
	r11_local_r11.u64 = REX_LOAD_U8(r31.u32 + 46);
	// slw r10,r26,r27
	r11_local_r10.u64 = r27.u8 & 0x20 ? 0 : (r26.u32 << (r27.u8 & 0x3F));
	// and r11,r10,r11
	r11_local_r11.u64 = r11_local_r10.u64 & r11_local_r11.u64;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(r11_local_r11.s32, 0, xer);
	// beq cr6,0x828c3bf8
	if (cr6.eq) goto loc_828C3BF8;
	// lwz r11,104(r1)
	r11_local_r11.u64 = REX_LOAD_U32(r11_local_r1.u32 + 104);
	// li r29,256
	r29.s64 = 256;
	// add r30,r28,r11
	r30.u64 = r28.u64 + r11_local_r11.u64;
#if REX_PLATFORM_NX || defined(REX_TEST_SWITCH_MEMORY)
	r11_contiguous_samples = (r30.u32 & 0xFFFFFFu) <= (0x1000000u - 1024u);
	r11_sample_host = r11_contiguous_samples ? REX_GUEST_HOST(r30.u32) : nullptr;
#endif
loc_828C3B30:
	// lbz r11,852(r31)
	r11_local_r11.u64 = REX_LOAD_U8(r31.u32 + 852);
	// lfs f0,32(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + 32);
	r11_local_f0.f64 = double(temp.f32);
	// lfs f13,0(r30)
	temp.u32 = r11_contiguous_samples ? __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(r11_sample_host)) : REX_LOAD_U32(r30.u32 + 0);
	ctx.f13.f64 = double(temp.f32);
	// fmuls f31,f0,f13
	f31.f64 = double(float(r11_local_f0.f64 * ctx.f13.f64));
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(r11_local_r11.u32, 0, xer);
	// beq cr6,0x828c3b68
	if (cr6.eq) goto loc_828C3B68;
	// lwz r3,40(r31)
	r11_local_r3.u64 = REX_LOAD_U32(r31.u32 + 40);
	// cmplwi cr6,r3,0
	cr6.compare<uint32_t>(r11_local_r3.u32, 0, xer);
	// beq cr6,0x828c3b68
	if (cr6.eq) goto loc_828C3B68;
	// fabs f1,f31
	r11_local_f1.u64 = f31.u64 & ~0x8000000000000000;
	// bl 0x8289ec00
	ctx.lr = 0x828C3B5C;
	r11_commit();
	__imp__sub_8289EC00(ctx, base);
	r11_reload();
	// fsel f0,f31,f28,f27
	ctx.fpscr.disableFlushMode();
	r11_local_f0.f64 = f31.f64 >= 0.0 ? f28.f64 : f27.f64;
	// fmuls f0,f1,f0
	r11_local_f0.f64 = double(float(r11_local_f1.f64 * r11_local_f0.f64));
	// b 0x828c3bd0
	goto loc_828C3BD0;
loc_828C3B68:
	// fsubs f0,f31,f27
	ctx.fpscr.disableFlushMode();
	r11_local_f0.f64 = double(float(f31.f64 - f27.f64));
	// addi r11,r1,80
	r11_local_r11.s64 = r11_local_r1.s64 + 80;
	// fsel f13,f0,f31,f30
	ctx.f13.f64 = r11_local_f0.f64 >= 0.0 ? f31.f64 : f30.f64;
	// fsel f0,f0,f31,f30
	r11_local_f0.f64 = r11_local_f0.f64 >= 0.0 ? f31.f64 : f30.f64;
	// fsub f13,f13,f29
	ctx.f13.f64 = ctx.f13.f64 - f29.f64;
	// fsel f0,f13,f29,f0
	r11_local_f0.f64 = ctx.f13.f64 >= 0.0 ? f29.f64 : r11_local_f0.f64;
	// frsp f31,f0
	f31.f64 = double(float(r11_local_f0.f64));
	// fadds f0,f31,f28
	r11_local_f0.f64 = double(float(f31.f64 + f28.f64));
	// fmuls f0,f0,f26
	r11_local_f0.f64 = double(float(r11_local_f0.f64 * f26.f64));
	// fctiwz f13,f0
	ctx.f13.s64 = std::isnan(r11_local_f0.f64) ? int64_t(0x80000000U) : (r11_local_f0.f64 > double(INT_MAX)) ? INT_MAX : simde_mm_cvttsd_si32(simde_mm_load_sd(&r11_local_f0.f64));
	// stfiwx f13,0,r11
	REX_STORE_U32(r11_local_r11.u32, ctx.f13.u32);
	// lwz r11,80(r1)
	r11_local_r11.u64 = REX_LOAD_U32(r11_local_r1.u32 + 80);
	// extsw r10,r11
	r11_local_r10.s64 = r11_local_r11.s32;
	// addi r9,r11,12
	r11_local_r9.s64 = r11_local_r11.s64 + 12;
	// addi r11,r11,13
	r11_local_r11.s64 = r11_local_r11.s64 + 13;
	// rlwinm r9,r9,2,0,29
	r11_local_r9.u64 = __builtin_rotateleft64(r11_local_r9.u32 | (r11_local_r9.u64 << 32), 2) & 0xFFFFFFFC;
	// rlwinm r11,r11,2,0,29
	r11_local_r11.u64 = __builtin_rotateleft64(r11_local_r11.u32 | (r11_local_r11.u64 << 32), 2) & 0xFFFFFFFC;
	// std r10,88(r1)
	REX_STORE_U64(r11_local_r1.u32 + 88, r11_local_r10.u64);
	// lfsx f13,r9,r31
	temp.u32 = REX_LOAD_U32(r11_local_r9.u32 + r31.u32);
	ctx.f13.f64 = double(temp.f32);
	// lfsx f12,r11,r31
	temp.u32 = REX_LOAD_U32(r11_local_r11.u32 + r31.u32);
	r11_local_f12.f64 = double(temp.f32);
	// fsubs f12,f12,f13
	r11_local_f12.f64 = double(float(r11_local_f12.f64 - ctx.f13.f64));
	// lfd f11,88(r1)
	r11_local_f11.u64 = REX_LOAD_U64(r11_local_r1.u32 + 88);
	// fcfid f11,f11
	r11_local_f11.f64 = double(r11_local_f11.s64);
	// frsp f11,f11
	r11_local_f11.f64 = double(float(r11_local_f11.f64));
	// fsubs f0,f0,f11
	r11_local_f0.f64 = double(float(r11_local_f0.f64 - r11_local_f11.f64));
	// fmadds f0,f12,f0,f13
	r11_local_f0.f64 = double(float(std::fma(r11_local_f12.f64, r11_local_f0.f64, ctx.f13.f64)));
loc_828C3BD0:
	// fsubs f0,f0,f31
	ctx.fpscr.disableFlushMode();
	r11_local_f0.f64 = double(float(r11_local_f0.f64 - f31.f64));
	// lfs f13,20(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 20);
	ctx.f13.f64 = double(temp.f32);
	// lfs f12,36(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 36);
	r11_local_f12.f64 = double(temp.f32);
	// addi r29,r29,-1
	r29.s64 = r29.s64 + -1;
	// cmplwi cr6,r29,0
	cr6.compare<uint32_t>(r29.u32, 0, xer);
	// fmadds f0,f0,f13,f31
	r11_local_f0.f64 = double(float(std::fma(r11_local_f0.f64, ctx.f13.f64, f31.f64)));
	// fmuls f0,f0,f12
	r11_local_f0.f64 = double(float(r11_local_f0.f64 * r11_local_f12.f64));
	// stfs f0,0(r30)
	temp.f32 = float(r11_local_f0.f64);
	if (r11_contiguous_samples) {
		*reinterpret_cast<volatile uint32_t*>(r11_sample_host) = __builtin_bswap32(temp.u32);
		r11_sample_host += 4;
	} else {
		REX_STORE_U32(r30.u32 + 0, temp.u32);
	}
	// addi r30,r30,4
	r30.s64 = r30.s64 + 4;
	// bne cr6,0x828c3b30
	if (!cr6.eq) goto loc_828C3B30;
loc_828C3BF8:
	// lbz r11,97(r1)
	r11_local_r11.u64 = REX_LOAD_U8(r11_local_r1.u32 + 97);
	// addi r27,r27,1
	r27.s64 = r27.s64 + 1;
	// addi r28,r28,1024
	r28.s64 = r28.s64 + 1024;
	// cmplw cr6,r27,r11
	cr6.compare<uint32_t>(r27.u32, r11_local_r11.u32, xer);
	// blt cr6,0x828c3b10
	if (cr6.lt) goto loc_828C3B10;
loc_828C3C0C:
	// li r3,0
	r11_local_r3.s64 = 0;
	// addi r1,r1,224
	r11_local_r1.s64 = r11_local_r1.s64 + 224;
	// addi r12,r1,-56
	r11_local_r12.s64 = r11_local_r1.s64 + -56;
	// bl 0x8299013c
	ctx.lr = 0x828C3C1C;
	// b 0x8298eb10
	{ r11_commit(); return; }
}


static inline __attribute__((always_inline)) void r11_exact_sub_82168520(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	// cmplwi cr6,r3,0
	cr6.compare<uint32_t>(ctx.r3.u32, 0, xer);
	// addi r11,r3,-8
	ctx.r11.s64 = ctx.r3.s64 + -8;
	// bne cr6,0x82168530
	if (!cr6.eq) goto loc_82168530;
	// li r11,0
	ctx.r11.s64 = 0;
loc_82168530:
	// addi r10,r11,20
	ctx.r10.s64 = ctx.r11.s64 + 20;
	// li r3,0
	ctx.r3.s64 = 0;
	// lwz r9,0(r10)
	ctx.r9.u64 = REX_LOAD_U32(ctx.r10.u32 + 0);
	// stw r9,0(r4)
	REX_STORE_U32(ctx.r4.u32 + 0, ctx.r9.u32);
	// lwz r10,4(r10)
	ctx.r10.u64 = REX_LOAD_U32(ctx.r10.u32 + 4);
	// stw r10,4(r4)
	REX_STORE_U32(ctx.r4.u32 + 4, ctx.r10.u32);
	// lwz r11,28(r11)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 28);
	// stw r11,8(r4)
	REX_STORE_U32(ctx.r4.u32 + 8, ctx.r11.u32);
	// blr 
	return;
}


static __attribute__((noinline)) void r11_curve_packet(PPCContext& __restrict ctx, uint8_t* base) {
	REX_FUNC_PROLOGUE();
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r26{};
	PPCRegister r27{};
	PPCRegister r28{};
	PPCRegister r29{};
	PPCRegister r30{};
	PPCRegister r31{};
	PPCRegister f26{};
	PPCRegister f27{};
	PPCRegister f28{};
	PPCRegister f29{};
	PPCRegister f30{};
	PPCRegister f31{};
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// bl 0x8298eac0
	ctx.lr = 0x828C3AA8;
	// addi r12,r1,-56
	ctx.r12.s64 = ctx.r1.s64 + -56;
	// bl 0x829900f0
	ctx.lr = 0x828C3AB0;
	// stwu r1,-224(r1)
	ea = -224 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// mr r31,r3
	r31.u64 = ctx.r3.u64;
	// mr r3,r4
	ctx.r3.u64 = ctx.r4.u64;
	// lbz r11,29(r31)
	ctx.r11.u64 = REX_LOAD_U8(r31.u32 + 29);
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(ctx.r11.u32, 0, xer);
	// bne cr6,0x828c3c0c
	if (!cr6.eq) goto loc_828C3C0C;
	// addi r4,r1,96
	ctx.r4.s64 = ctx.r1.s64 + 96;
	// bl 0x82168520
	ctx.lr = 0x828C3AD0;
	__imp__sub_82168520(ctx, base);
	// lbz r11,97(r1)
	ctx.r11.u64 = REX_LOAD_U8(ctx.r1.u32 + 97);
	// li r27,0
	r27.s64 = 0;
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(ctx.r11.u32, 0, xer);
	// beq cr6,0x828c3c0c
	if (cr6.eq) goto loc_828C3C0C;
	// lis r7,-32256
	ctx.r7.s64 = -2113929216;
	// lis r8,-32245
	ctx.r8.s64 = -2113208320;
	// lis r9,-32256
	ctx.r9.s64 = -2113929216;
	// lis r10,-32245
	ctx.r10.s64 = -2113208320;
	// lis r11,-32256
	ctx.r11.s64 = -2113929216;
	// lfs f26,9928(r7)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r7.u32 + 9928);
	f26.f64 = double(temp.f32);
	// li r28,0
	r28.s64 = 0;
	// lfd f29,-6912(r8)
	f29.u64 = REX_LOAD_U64(ctx.r8.u32 + -6912);
	// li r26,1
	r26.s64 = 1;
	// lfd f30,9920(r9)
	f30.u64 = REX_LOAD_U64(ctx.r9.u32 + 9920);
	// lfs f27,-6944(r10)
	temp.u32 = REX_LOAD_U32(ctx.r10.u32 + -6944);
	f27.f64 = double(temp.f32);
	// lfs f28,2592(r11)
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + 2592);
	f28.f64 = double(temp.f32);
loc_828C3B10:
	// lbz r11,46(r31)
	ctx.r11.u64 = REX_LOAD_U8(r31.u32 + 46);
	// slw r10,r26,r27
	ctx.r10.u64 = r27.u8 & 0x20 ? 0 : (r26.u32 << (r27.u8 & 0x3F));
	// and r11,r10,r11
	ctx.r11.u64 = ctx.r10.u64 & ctx.r11.u64;
	// cmpwi cr6,r11,0
	cr6.compare<int32_t>(ctx.r11.s32, 0, xer);
	// beq cr6,0x828c3bf8
	if (cr6.eq) goto loc_828C3BF8;
	// lwz r11,104(r1)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 104);
	// li r29,256
	r29.s64 = 256;
	// add r30,r28,r11
	r30.u64 = r28.u64 + ctx.r11.u64;
loc_828C3B30:
	// lbz r11,852(r31)
	ctx.r11.u64 = REX_LOAD_U8(r31.u32 + 852);
	// lfs f0,32(r31)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(r31.u32 + 32);
	ctx.f0.f64 = double(temp.f32);
	// lfs f13,0(r30)
	temp.u32 = REX_LOAD_U32(r30.u32 + 0);
	ctx.f13.f64 = double(temp.f32);
	// fmuls f31,f0,f13
	f31.f64 = double(float(ctx.f0.f64 * ctx.f13.f64));
	// cmplwi cr6,r11,0
	cr6.compare<uint32_t>(ctx.r11.u32, 0, xer);
	// beq cr6,0x828c3b68
	if (cr6.eq) goto loc_828C3B68;
	// lwz r3,40(r31)
	ctx.r3.u64 = REX_LOAD_U32(r31.u32 + 40);
	// cmplwi cr6,r3,0
	cr6.compare<uint32_t>(ctx.r3.u32, 0, xer);
	// beq cr6,0x828c3b68
	if (cr6.eq) goto loc_828C3B68;
	// fabs f1,f31
	ctx.f1.u64 = f31.u64 & ~0x8000000000000000;
	// bl 0x8289ec00
	ctx.lr = 0x828C3B5C;
	sub_8289EC00(ctx, base);
	// fsel f0,f31,f28,f27
	ctx.fpscr.disableFlushMode();
	ctx.f0.f64 = f31.f64 >= 0.0 ? f28.f64 : f27.f64;
	// fmuls f0,f1,f0
	ctx.f0.f64 = double(float(ctx.f1.f64 * ctx.f0.f64));
	// b 0x828c3bd0
	goto loc_828C3BD0;
loc_828C3B68:
	// fsubs f0,f31,f27
	ctx.fpscr.disableFlushMode();
	ctx.f0.f64 = double(float(f31.f64 - f27.f64));
	// addi r11,r1,80
	ctx.r11.s64 = ctx.r1.s64 + 80;
	// fsel f13,f0,f31,f30
	ctx.f13.f64 = ctx.f0.f64 >= 0.0 ? f31.f64 : f30.f64;
	// fsel f0,f0,f31,f30
	ctx.f0.f64 = ctx.f0.f64 >= 0.0 ? f31.f64 : f30.f64;
	// fsub f13,f13,f29
	ctx.f13.f64 = ctx.f13.f64 - f29.f64;
	// fsel f0,f13,f29,f0
	ctx.f0.f64 = ctx.f13.f64 >= 0.0 ? f29.f64 : ctx.f0.f64;
	// frsp f31,f0
	f31.f64 = double(float(ctx.f0.f64));
	// fadds f0,f31,f28
	ctx.f0.f64 = double(float(f31.f64 + f28.f64));
	// fmuls f0,f0,f26
	ctx.f0.f64 = double(float(ctx.f0.f64 * f26.f64));
	// fctiwz f13,f0
	ctx.f13.s64 = std::isnan(ctx.f0.f64) ? int64_t(0x80000000U) : (ctx.f0.f64 > double(INT_MAX)) ? INT_MAX : simde_mm_cvttsd_si32(simde_mm_load_sd(&ctx.f0.f64));
	// stfiwx f13,0,r11
	REX_STORE_U32(ctx.r11.u32, ctx.f13.u32);
	// lwz r11,80(r1)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 80);
	// extsw r10,r11
	ctx.r10.s64 = ctx.r11.s32;
	// addi r9,r11,12
	ctx.r9.s64 = ctx.r11.s64 + 12;
	// addi r11,r11,13
	ctx.r11.s64 = ctx.r11.s64 + 13;
	// rlwinm r9,r9,2,0,29
	ctx.r9.u64 = __builtin_rotateleft64(ctx.r9.u32 | (ctx.r9.u64 << 32), 2) & 0xFFFFFFFC;
	// rlwinm r11,r11,2,0,29
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 2) & 0xFFFFFFFC;
	// std r10,88(r1)
	REX_STORE_U64(ctx.r1.u32 + 88, ctx.r10.u64);
	// lfsx f13,r9,r31
	temp.u32 = REX_LOAD_U32(ctx.r9.u32 + r31.u32);
	ctx.f13.f64 = double(temp.f32);
	// lfsx f12,r11,r31
	temp.u32 = REX_LOAD_U32(ctx.r11.u32 + r31.u32);
	ctx.f12.f64 = double(temp.f32);
	// fsubs f12,f12,f13
	ctx.f12.f64 = double(float(ctx.f12.f64 - ctx.f13.f64));
	// lfd f11,88(r1)
	ctx.f11.u64 = REX_LOAD_U64(ctx.r1.u32 + 88);
	// fcfid f11,f11
	ctx.f11.f64 = double(ctx.f11.s64);
	// frsp f11,f11
	ctx.f11.f64 = double(float(ctx.f11.f64));
	// fsubs f0,f0,f11
	ctx.f0.f64 = double(float(ctx.f0.f64 - ctx.f11.f64));
	// fmadds f0,f12,f0,f13
	ctx.f0.f64 = double(float(std::fma(ctx.f12.f64, ctx.f0.f64, ctx.f13.f64)));
loc_828C3BD0:
	// fsubs f0,f0,f31
	ctx.fpscr.disableFlushMode();
	ctx.f0.f64 = double(float(ctx.f0.f64 - f31.f64));
	// lfs f13,20(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 20);
	ctx.f13.f64 = double(temp.f32);
	// lfs f12,36(r31)
	temp.u32 = REX_LOAD_U32(r31.u32 + 36);
	ctx.f12.f64 = double(temp.f32);
	// addi r29,r29,-1
	r29.s64 = r29.s64 + -1;
	// cmplwi cr6,r29,0
	cr6.compare<uint32_t>(r29.u32, 0, xer);
	// fmadds f0,f0,f13,f31
	ctx.f0.f64 = double(float(std::fma(ctx.f0.f64, ctx.f13.f64, f31.f64)));
	// fmuls f0,f0,f12
	ctx.f0.f64 = double(float(ctx.f0.f64 * ctx.f12.f64));
	// stfs f0,0(r30)
	temp.f32 = float(ctx.f0.f64);
	REX_STORE_U32(r30.u32 + 0, temp.u32);
	// addi r30,r30,4
	r30.s64 = r30.s64 + 4;
	// bne cr6,0x828c3b30
	if (!cr6.eq) goto loc_828C3B30;
loc_828C3BF8:
	// lbz r11,97(r1)
	ctx.r11.u64 = REX_LOAD_U8(ctx.r1.u32 + 97);
	// addi r27,r27,1
	r27.s64 = r27.s64 + 1;
	// addi r28,r28,1024
	r28.s64 = r28.s64 + 1024;
	// cmplw cr6,r27,r11
	cr6.compare<uint32_t>(r27.u32, ctx.r11.u32, xer);
	// blt cr6,0x828c3b10
	if (cr6.lt) goto loc_828C3B10;
loc_828C3C0C:
	// li r3,0
	ctx.r3.s64 = 0;
	// addi r1,r1,224
	ctx.r1.s64 = ctx.r1.s64 + 224;
	// addi r12,r1,-56
	ctx.r12.s64 = ctx.r1.s64 + -56;
	// bl 0x8299013c
	ctx.lr = 0x828C3C1C;
	// b 0x8298eb10
	return;
}


static inline __attribute__((always_inline)) void r11_packet_select(PPCContext& __restrict ctx, uint8_t* base) {
	if (REX_LOAD_U8(ctx.r3.u32 + 852)) { r11_curve_packet(ctx, base); return; }
	r11_exact_sub_828C3AA0(ctx, base);
}
