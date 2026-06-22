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

    NSString* pathFromAppleEventDescriptor(NSAppleEventDescriptor* item)
    {
        if (!item)
            return nil;

        NSURL* url = [item fileURLValue];
        if (url)
            return [url path];

        NSAppleEventDescriptor* fileURL =
            [item coerceToDescriptorType:typeFileURL];
        if (fileURL)
        {
            url = [fileURL fileURLValue];
            if (url)
                return [url path];

            NSString* urlString = [fileURL stringValue];
            if (urlString)
                return [[NSURL URLWithString:urlString] path];
        }

        return [item stringValue];
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

    void mrvApplicationOpenURLs(
        id self, SEL _cmd, NSApplication* application, NSArray* urls)
    {
        (void)self;
        (void)_cmd;
        (void)application;

        for (NSURL* url in urls)
        {
            if ([url isFileURL])
                openPath([url path]);
        }
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

        class_addMethod(
            delegateClass, @selector(application:openURLs:),
            (IMP)mrvApplicationOpenURLs, "v@:@@");

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

    NSInteger count = [directObject numberOfItems];
    if (count <= 0)
    {
        NSString* path = pathFromAppleEventDescriptor(directObject);
        if (path)
            openPath(path);
        return;
    }

    for (NSInteger i = 1; i <= count; ++i)
    {
        NSAppleEventDescriptor* item = [directObject descriptorAtIndex:i];
        if (!item)
            continue;

        NSString* path = pathFromAppleEventDescriptor(item);
        if (path)
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
