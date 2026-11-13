// SPDX-License-Identifier: MIT
// darwin_abi: do two compilers pass C++ records by value the same way on macOS? Built twice: the callee by Apple's
// clang (the compiler of libsycl, libc++ and every other C++ library of the platform), the caller by the DPC++ clang
// (the compiler of SYCL programs). Each record holds 4 and 2 and the callee returns 42 when it reads them where the
// caller put them. The records are the cases where clang's C++11 rule and its Clang 4 rule (Apple's clang on Darwin)
// differ, and one where both pass by address. 13 Nov: the DPC++ clang without intel-llvm patch 0006 gave "BAD" for the
// first five (DESIGN-sycl.md section 3.1: sycl::detail::type_erased_cgfo_ty is the first kind).
//   xcrun clang++ -std=c++17 -O2 -arch x86_64 -DCALLEE -c darwin_abi.cpp -o callee.o
//   $SYCL_WORK/build/bin/clang++ -std=c++17 -O2 -mmacosx-version-min=15.0 --ld-path=$(xcrun -f ld) darwin_abi.cpp \
//     callee.o -o darwin_abi && ./darwin_abi     # six "ok", exit 0
#define F                                                                                                              \
	long a, b;
struct A1 { // copy and move deleted (type_erased_cgfo_ty)
	F A1(long x, long y) : a(x), b(y) {}
	A1(const A1 &) = delete;
	A1(A1 &&) = delete;
};
struct A4 { // copy deleted, move user-provided
	F A4(long x, long y) : a(x), b(y) {}
	A4(const A4 &) = delete;
	A4(A4 &&);
};
struct A8 { // copy deleted, move not declared
	F A8(long x, long y) : a(x), b(y) {}
	A8(const A8 &) = delete;
};
struct A9 { // move deleted (copy implicitly deleted)
	F A9(long x, long y) : a(x), b(y) {}
	A9(A9 &&) = delete;
};
struct A10 { // trivial copy, move user-provided
	F A10(long x, long y) : a(x), b(y) {}
	A10(const A10 &) = default;
	A10(A10 &&);
};
struct A5 { // copy user-provided: by address under both rules
	F A5(long x, long y) : a(x), b(y) {}
	A5(const A5 &);
	A5(A5 &&) = default;
};
long f1(int, A1);
long f4(int, A4);
long f8(int, A8);
long f9(int, A9);
long f10(int, A10);
long f5(int, A5);

#ifdef CALLEE
A4::A4(A4 &&o) : a(o.a), b(o.b) {}
A10::A10(A10 &&o) : a(o.a), b(o.b) {}
A5::A5(const A5 &o) : a(o.a), b(o.b) {}
long f1(int, A1 r) { return r.a * 10 + r.b; }
long f4(int, A4 r) { return r.a * 10 + r.b; }
long f8(int, A8 r) { return r.a * 10 + r.b; }
long f9(int, A9 r) { return r.a * 10 + r.b; }
long f10(int, A10 r) { return r.a * 10 + r.b; }
long f5(int, A5 r) { return r.a * 10 + r.b; }
#else
#include <cstdio>
int main()
{
	const long r[] = {f1(0, A1(4, 2)), f4(0, A4(4, 2)), f8(0, A8(4, 2)), f9(0, A9(4, 2)), f10(0, A10(4, 2)), f5(0, A5(4, 2))};
	const char *n[] = {"copy and move deleted", "copy deleted, user move", "copy deleted, no move", "move deleted",
			   "trivial copy, user move", "user copy"};
	int bad = 0;
	for (int i = 0; i < 6; i++) {
		printf("%s %s (%ld)\n", r[i] == 42 ? "ok " : "BAD", n[i], r[i]);
		bad += r[i] != 42;
	}
	return bad;
}
#endif
