// SPDX-License-Identifier: BSD-3-Clause
// mrv2
// Copyright Contributors to the mrv2 Project. All rights reserved.

#include "mrvApp/mrvMacOSOpen.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

namespace
{
    mrv::MacOSOpenCallback gOpenCallback = nullptr;
    Class gSwizzledDelegateClass = Nil;

    void openPath(NSString* path)
    {
        if (!gOpenCallback || !path)
            return;

        gOpenCallback([path fileSystemRepresentation]);
    }

    BOOL mrvApplicationOpenFile(
        id self, SEL _cmd, NSApplication* application, NSString* filename)
    {
        (void)self;
        (void)_cmd;
        (void)application;

        openPath(filename);
        return YES;
    }

    void mrvApplicationOpenFiles(
        id self, SEL _cmd, NSApplication* application, NSArray* filenames)
    {
        (void)self;
        (void)_cmd;

        for (NSString* filename in filenames)
            openPath(filename);

        [application replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
    }

    void installDelegateOpenHandlers()
    {
        id<NSApplicationDelegate> delegate = [NSApp delegate];
        if (!delegate)
            return;

        Class delegateClass = object_getClass(delegate);
        if (!delegateClass || delegateClass == gSwizzledDelegateClass)
            return;

        SEL openFileSelector = @selector(application:openFile:);
        Method openFileMethod =
            class_getInstanceMethod(delegateClass, openFileSelector);
        if (openFileMethod)
        {
            method_setImplementation(
                openFileMethod, (IMP)mrvApplicationOpenFile);
        }
        else
        {
            class_addMethod(
                delegateClass, openFileSelector,
                (IMP)mrvApplicationOpenFile, "B@:@@");
        }

        class_addMethod(
            delegateClass, @selector(application:openFiles:),
            (IMP)mrvApplicationOpenFiles, "v@:@@");

        gSwizzledDelegateClass = delegateClass;
    }
}

@interface MRVMacOSOpenHandler : NSObject
- (void)handleOpenDocuments:(NSAppleEventDescriptor*)event
             withReplyEvent:(NSAppleEventDescriptor*)replyEvent;
@end

@implementation MRVMacOSOpenHandler

- (void)handleOpenDocuments:(NSAppleEventDescriptor*)event
             withReplyEvent:(NSAppleEventDescriptor*)replyEvent
{
    (void)replyEvent;

    NSAppleEventDescriptor* directObject =
        [event paramDescriptorForKeyword:keyDirectObject];
    if (!directObject)
        return;

    for (NSInteger i = 1; i <= [directObject numberOfItems]; ++i)
    {
        NSAppleEventDescriptor* item = [directObject descriptorAtIndex:i];
        if (!item)
            continue;

        NSString* path = nil;
        NSAppleEventDescriptor* fileURL =
            [item coerceToDescriptorType:typeFileURL];
        if (fileURL)
        {
            NSString* urlString = [fileURL stringValue];
            if (urlString)
            {
                NSURL* url = [NSURL URLWithString:urlString];
                path = [url path];
            }
        }

        if (!path)
            path = [item stringValue];

        openPath(path);
    }
}

@end

namespace mrv
{
    void installMacOSOpenDocumentHandler(MacOSOpenCallback callback)
    {
        gOpenCallback = callback;

        installDelegateOpenHandlers();

        static MRVMacOSOpenHandler* handler = nil;
        if (!handler)
            handler = [[MRVMacOSOpenHandler alloc] init];

        [[NSAppleEventManager sharedAppleEventManager]
            setEventHandler:handler
                andSelector:@selector(handleOpenDocuments:withReplyEvent:)
              forEventClass:kCoreEventClass
                 andEventID:kAEOpenDocuments];
    }
}
