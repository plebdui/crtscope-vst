// A tiny Audio Unit host for macOS CI: opens CRT Scope's window the way Logic
// does (Cocoa view factory), pushes a stereo test tone through it for a few
// seconds, and lets the plugin save snapshots of its window (CRTSCOPE_DUMP).
//   clang++ -fobjc-arc -std=c++17 mac_auhost.mm -framework Cocoa -framework AudioToolbox -framework AudioUnit -framework CoreAudioKit -o mac_auhost
#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>
#include <cmath>
#include <cstdio>

static double gPhase = 0;

static OSStatus feed(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 n, AudioBufferList* io)
{
    float* l = (float*)io->mBuffers[0].mData;
    float* r = io->mNumberBuffers > 1 ? (float*)io->mBuffers[1].mData : l;
    for (UInt32 i = 0; i < n; ++i) {
        l[i] = 0.6f * std::sin(gPhase);
        r[i] = 0.6f * std::sin(gPhase * 1.5 + 0.5);
        gPhase += 2 * M_PI * 220 / 48000.0;
    }
    return noErr;
}

#define CHECK(c, m) do { if (!(c)) { std::printf("FAIL: %s\n", m); return 1; } } while (0)

int main()
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        AudioComponentDescription d = {kAudioUnitType_Effect, 'CrtS', 'Pleb', 0, 0};
        AudioComponent comp = AudioComponentFindNext(nullptr, &d);
        CHECK(comp, "find the component");
        AudioUnit au = nullptr;
        CHECK(AudioComponentInstanceNew(comp, &au) == noErr, "instantiate");

        AudioStreamBasicDescription fmt = {48000, kAudioFormatLinearPCM,
            kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved, 4, 1, 4, 2, 32, 0};
        AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
        AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &fmt, sizeof(fmt));
        AURenderCallbackStruct cb = {feed, nullptr};
        AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
        CHECK(AudioUnitInitialize(au) == noErr, "initialize");

        // the window, the way hosts ask for it
        UInt32 size = 0;
        Boolean writable = false;
        CHECK(AudioUnitGetPropertyInfo(au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &size, &writable) == noErr, "has a Cocoa view");
        AudioUnitCocoaViewInfo* info = (AudioUnitCocoaViewInfo*)malloc(size);
        CHECK(AudioUnitGetProperty(au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, info, &size) == noErr, "Cocoa view info");
        NSBundle* bundle = [NSBundle bundleWithURL:(__bridge NSURL*)info->mCocoaAUViewBundleLocation];
        Class factoryClass = [bundle classNamed:(__bridge NSString*)info->mCocoaAUViewClass[0]];
        CHECK(factoryClass, "view factory class");
        id<AUCocoaUIBase> factory = [[factoryClass alloc] init];
        NSView* view = [factory uiViewForAudioUnit:au withSize:NSMakeSize(640, 724)];
        CHECK(view, "plugin view");
        std::printf("plugin view %.0fx%.0f\n", view.frame.size.width, view.frame.size.height);
        NSWindow* win = [[NSWindow alloc] initWithContentRect:view.frame
                                                    styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        [win setContentView:view];
        [win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        // run for 9 seconds, rendering audio like a host would
        float l[512], r[512];
        int blocks = 0;
        bool passthrough = true;
        NSDate* end = [NSDate dateWithTimeIntervalSinceNow:9];
        while ([end timeIntervalSinceNow] > 0) {
            AudioBufferList* abl = (AudioBufferList*)alloca(sizeof(AudioBufferList) + sizeof(AudioBuffer));
            abl->mNumberBuffers = 2;
            abl->mBuffers[0] = {1, sizeof(l), l};
            abl->mBuffers[1] = {1, sizeof(r), r};
            AudioUnitRenderActionFlags flags = 0;
            AudioTimeStamp ts = {};
            ts.mFlags = kAudioTimeStampSampleTimeValid;
            ts.mSampleTime = blocks * 512.0;
            const double before = gPhase;
            if (AudioUnitRender(au, &flags, &ts, 0, 512, abl) != noErr) { passthrough = false; break; }
            // flaws are off by default, so the tone must come out untouched
            double ph = before;
            for (int i = 0; i < 512; ++i, ph += 2 * M_PI * 220 / 48000.0)
                if (std::fabs(l[i] - (float)(0.6 * std::sin(ph))) > 1e-6f) passthrough = false;
            ++blocks;
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:512 / 48000.0]];
        }
        std::printf("audio blocks: %d, passthrough %s\n", blocks, passthrough ? "exact" : "WRONG");
        [win orderOut:nil];
        AudioUnitUninitialize(au);
        AudioComponentInstanceDispose(au);
        CHECK(passthrough && blocks > 100, "audio through the AU");
        std::printf("ALL OK\n");
    }
    return 0;
}
