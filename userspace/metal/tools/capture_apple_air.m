// MIT license. Compile our authored MSL with the system Metal compiler and
// retain its actual AIR for compiler interoperability testing. This tool is
// diagnostic only: bitcodeData is private and is never assumed to exist.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 3) {
            fprintf(stderr, "usage: capture-apple-air INPUT.metal OUTPUT.air\n");
            return 2;
        }
        NSError *error = nil;
        NSString *source = [NSString stringWithContentsOfFile:@(argv[1])
            encoding:NSUTF8StringEncoding error:&error];
        if (!source) {
            fprintf(stderr, "read MSL: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            fprintf(stderr, "No system Metal device available; no replacement input generated\n");
            return 1;
        }
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_4;
        options.fastMathEnabled = NO;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        if (!library) {
            fprintf(stderr, "Apple MSL compilation failed: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        id<MTLFunction> function = [library newFunctionWithName:@"add_one"];
        SEL selector = NSSelectorFromString(@"bitcodeData");
        if (!function || ![function respondsToSelector:selector]) {
            fprintf(stderr, "Apple function exposes no bitcodeData; capture unsupported on this OS\n");
            return 1;
        }
        id payload = ((id (*)(id, SEL))objc_msgSend)(function, selector);
        if (![payload isKindOfClass:[NSData class]] || [payload length] == 0) {
            fprintf(stderr, "Apple function returned no AIR bytes\n");
            return 1;
        }
        NSData *air = payload;
        if (![air writeToFile:@(argv[2]) options:NSDataWritingAtomic error:&error]) {
            fprintf(stderr, "write AIR: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        printf("Apple runtime compiler: device=%s; function=%s; MSL=2.4; AIR=%lu bytes\n",
            device.name.UTF8String, function.name.UTF8String, (unsigned long)air.length);
        return 0;
    }
}
