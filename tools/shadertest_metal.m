/* shadertest's Metal check: compiles MSL with the system's Metal compiler (the
 * one the Metal backend uses at run time), so it needs no Xcode toolchain. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdbool.h>
#include <stdio.h>

bool shadertest_metal_compile(const char *src, bool atest, char *err, size_t errlen) {
    @autoreleasepool {
        static id<MTLDevice> dev;
        if (!dev) dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            snprintf(err, errlen, "no Metal device");
            return false;
        }
        MTLCompileOptions *opt = [MTLCompileOptions new];
        if (atest) opt.preprocessorMacros = @{@"X2_ATEST" : @1};
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:src] options:opt error:&e];
        if (!lib) {
            snprintf(err, errlen, "%s", e.localizedDescription.UTF8String);
            return false;
        }
        if (![lib newFunctionWithName:@"x2_main"]) {
            snprintf(err, errlen, "no x2_main");
            return false;
        }
        return true;
    }
}
