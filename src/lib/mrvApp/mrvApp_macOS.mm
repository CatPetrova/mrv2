// SPDX-License-Identifier: BSD-3-Clause
// mrv2
// Copyright Contributors to the mrv2 Project. All rights reserved.

#include "mrvFl/mrvInit.h"

#include <string>
#include <FL/platform.H>
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

namespace mrv
{
    void osx_open_cb(const char*);

    namespace
    {
        BOOL open_file(id, SEL, NSApplication*, NSString* file)
        {
            osx_open_cb([file UTF8String]);
            return YES;
        }

        void open_files(id, SEL, NSApplication* app, NSArray<NSString*>* files)
        {
            for (NSString* file in files)
                osx_open_cb([file UTF8String]);
            [app replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
        }

        void open_urls(id, SEL, NSApplication*, NSArray<NSURL*>* urls)
        {
            for (NSURL* url in urls)
            {
                if ([url isFileURL])
                    osx_open_cb([[url path] UTF8String]);
            }
        }

        void install_document_methods(Class delegate)
        {
            if (!delegate)
                return;
            // Preserve implementations supplied by newer toolkit releases.
            if (!class_getInstanceMethod(delegate, @selector(application:openFile:)))
            {
                const std::string signature = std::string(@encode(BOOL)) + "@:@@";
                class_addMethod(delegate, @selector(application:openFile:),
                                reinterpret_cast<IMP>(open_file), signature.c_str());
            }
            if (!class_getInstanceMethod(delegate, @selector(application:openFiles:)))
                class_addMethod(delegate, @selector(application:openFiles:),
                                reinterpret_cast<IMP>(open_files), "v@:@@");
            if (!class_getInstanceMethod(delegate, @selector(application:openURLs:)))
                class_addMethod(delegate, @selector(application:openURLs:),
                                reinterpret_cast<IMP>(open_urls), "v@:@@");
        }
    }

    void init_macos(const std::shared_ptr<tl::system::Context>& context)
    {
        // Both toolkits may dispatch launch events while they own NSApp's
        // delegate. Older FLTK only implements openFile, and GLFW has no
        // document callbacks. Route all three Cocoa entry points to our queue
        // before either toolkit can process the initial Finder document.
        install_document_methods(objc_getClass("FLAppDelegate"));
        install_document_methods(objc_getClass("GLFWApplicationDelegate"));
        fl_open_callback(osx_open_cb);
        id fltkDelegate = [[NSApp delegate] retain];
        mrv::init(context);
        // GLFW creates helper windows; FLTK handles application documents.
        [NSApp setDelegate:fltkDelegate];
        [fltkDelegate release];
    }
}
