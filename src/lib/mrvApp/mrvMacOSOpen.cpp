// SPDX-License-Identifier: BSD-3-Clause
// mrv2
// Copyright Contributors to the mrv2 Project. All rights reserved.

#include "mrvApp/mrvMacOSOpen.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

namespace
{
    mrv::MacOSOpenCallback gOpenCallback = nullptr;

    void openPath(NSString* path)
    {
        if (!gOpenCallback || !path)
            return;

        gOpenCallback([path fileSystemRepresentation]);
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
