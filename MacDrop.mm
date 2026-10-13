// macOS: files dragged from Finder onto the plugin window. DPF's window layer
// (Pugl) doesn't take drops, so the drag methods are added at run time to its
// view class. The class name is unique to this plugin build, so no other
// plugin in the same host is touched.
#import <Cocoa/Cocoa.h>
#include <objc/runtime.h>
#include "MacDrop.h"

static char kCallbackKey;

struct DropCallback {
    void (*fn)(void* ctx, const char* path);
    void* ctx;
};

static NSURL* firstFile(id<NSDraggingInfo> info)
{
    NSArray* urls = [[info draggingPasteboard] readObjectsForClasses:@[[NSURL class]]
                                                             options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    return urls.count ? urls[0] : nil;
}

static NSDragOperation dragEntered(id, SEL, id<NSDraggingInfo> info)
{
    return firstFile(info) ? NSDragOperationCopy : NSDragOperationNone;
}

static BOOL performDrag(id self, SEL, id<NSDraggingInfo> info)
{
    NSURL* url = firstFile(info);
    NSValue* box = objc_getAssociatedObject(self, &kCallbackKey);
    if (!url || !box)
        return NO;
    const DropCallback* cb = (const DropCallback*)[box pointerValue];
    cb->fn(cb->ctx, [[url path] fileSystemRepresentation]);
    return YES;
}

void crtMacDropRegister(void* nsview, void (*fn)(void* ctx, const char* path), void* ctx)
{
    NSView* view = (NSView*)nsview;
    if (!view)
        return;
    Class cls = object_getClass(view);
    class_addMethod(cls, @selector(draggingEntered:), (IMP)dragEntered, "Q@:@");
    class_addMethod(cls, @selector(draggingUpdated:), (IMP)dragEntered, "Q@:@");
    class_addMethod(cls, @selector(performDragOperation:), (IMP)performDrag, "c@:@");
    DropCallback* cb = new DropCallback{fn, ctx};
    objc_setAssociatedObject(view, &kCallbackKey, [NSValue valueWithPointer:cb], OBJC_ASSOCIATION_RETAIN);
    [view registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
}

void crtMacDropUnregister(void* nsview)
{
    NSView* view = (NSView*)nsview;
    if (!view)
        return;
    [view unregisterDraggedTypes];
    NSValue* box = objc_getAssociatedObject(view, &kCallbackKey);
    objc_setAssociatedObject(view, &kCallbackKey, nil, OBJC_ASSOCIATION_RETAIN);
    if (box)
        delete (DropCallback*)[box pointerValue];
}
