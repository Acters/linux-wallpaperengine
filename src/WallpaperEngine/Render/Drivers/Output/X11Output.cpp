#include "GLFWOutputViewport.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Drivers/GLFWOpenGLDriver.h"
#include "X11Output.h"

#include <algorithm>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#ifdef HAVE_XFIXES
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#endif

using namespace WallpaperEngine::Render::Drivers::Output;

void CustomXIOErrorExitHandler (Display* dsp, void* userdata) {
    const auto context = static_cast<X11Output*> (userdata);

    sLog.debugerror ("Critical XServer error detected. Attempting to recover...");

    // refetch all the resources
    context->reset ();
}

int CustomXErrorHandler (Display* dpy, XErrorEvent* event) {
    sLog.debugerror ("Detected X error");

    return 0;
}

int CustomXIOErrorHandler (Display* dsp) {
    sLog.debugerror ("Detected X error");

    return 0;
}

X11Output::X11Output (ApplicationContext& context, VideoDriver& driver) : Output (context, driver),
    m_display (nullptr),
    m_pixmap (None),
    m_root (None),
    m_gc (None),
    m_imageData (nullptr),
    m_imageSize (0),
    m_image (nullptr) {
    // do not use previous handler, it might stop the app under weird circumstances
    XSetErrorHandler (CustomXErrorHandler);
    XSetIOErrorHandler (CustomXIOErrorHandler);

    this->loadScreenInfo ();
}

X11Output::~X11Output () {
    this->free ();
}

void X11Output::reset () {
    // first free whatever we have right now
    this->free ();
    // re-load screen info
    this->loadScreenInfo ();
    // do the same for the detector
    // TODO: BRING BACK THIS FUNCTIONALITY
    // this->m_driver.getFullscreenDetector ().reset ();
}

void X11Output::free () {
    // go through all the viewports and free them
    for (const auto& [screen, viewport] : this->m_viewports)
        delete viewport;

    this->m_viewports.clear ();

    if (this->m_display != nullptr) {
        for (const auto& [name, gc] : this->m_windowGCs) {
            if (gc != None)
                XFreeGC (this->m_display, gc);
        }

        for (const auto& [name, window] : this->m_windows) {
            if (window != None)
                XDestroyWindow (this->m_display, window);
        }
    }

    this->m_windowGCs.clear ();
    this->m_windows.clear ();

    // free all the resources we've got
    if (this->m_image != nullptr)
        XDestroyImage (this->m_image);

    if (this->m_display != nullptr && this->m_gc != None)
        XFreeGC (this->m_display, this->m_gc);

    if (this->m_display != nullptr && this->m_pixmap != None)
        XFreePixmap (this->m_display, this->m_pixmap);

    delete [] this->m_imageData;

    if (this->m_display != nullptr)
        XCloseDisplay (this->m_display);
}

void* X11Output::getImageBuffer () const {
    return this->m_imageData;
}

bool X11Output::renderVFlip () const {
    return false;
}

bool X11Output::renderMultiple () const {
    return this->m_viewports.size () > 1;
}

bool X11Output::haveImageBuffer () const {
    return true;
}

uint32_t X11Output::getImageBufferSize () const {
    return this->m_imageSize;
}

void X11Output::loadScreenInfo () {
    this->m_display = XOpenDisplay (nullptr);
    // set the error handling to try and recover from X disconnections
#ifdef HAVE_XSETIOERROREXITHANDLER
    XSetIOErrorExitHandler (this->m_display, CustomXIOErrorExitHandler, this);
#endif /* HAVE_XSETIOERROREXITHANDLER */

    int xrandr_result, xrandr_error;

    if (!XRRQueryExtension (this->m_display, &xrandr_result, &xrandr_error)) {
        sLog.error ("XRandr is not present, cannot detect specified screens, running in window mode");
        return;
    }

    this->m_root = DefaultRootWindow (this->m_display);
    this->m_rootWidth = DisplayWidth (this->m_display, DefaultScreen (this->m_display));
    this->m_rootHeight = DisplayHeight (this->m_display, DefaultScreen (this->m_display));
    sLog.out ("X11 root size: ", this->m_rootWidth, "x", this->m_rootHeight);
    XRRScreenResources* screenResources = XRRGetScreenResources (this->m_display, DefaultRootWindow (this->m_display));

    if (screenResources == nullptr) {
        sLog.error ("Cannot detect screen sizes using xrandr, running in window mode");
        return;
    }

    bool haveRequestedBounds = false;
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;

    for (int i = 0; i < screenResources->noutput; i++) {
        const XRROutputInfo* info = XRRGetOutputInfo (this->m_display, screenResources, screenResources->outputs [i]);

        // screen not in use, ignore it
        if (info == nullptr || info->connection != RR_Connected)
            continue;

        XRRCrtcInfo* crtc = XRRGetCrtcInfo (this->m_display, screenResources, info->crtc);

        // screen not active, ignore it
        if (crtc == nullptr)
            continue;

        // add the screen to the list of screens
        this->m_screens.push_back (new GLFWOutputViewport {{crtc->x, crtc->y, crtc->width, crtc->height}, info->name});

        // only keep info of registered screens
        if (this->m_context.settings.general.screenBackgrounds.find (info->name) !=
            this->m_context.settings.general.screenBackgrounds.end ()) {
            sLog.out ("Found requested screen: ", info->name, " -> ", crtc->x, "x", crtc->y, ":", crtc->width, "x",
                      crtc->height);

            this->m_viewports [info->name] =
                new GLFWOutputViewport {{crtc->x, crtc->y, crtc->width, crtc->height}, info->name};

            const int screenMinX = crtc->x;
            const int screenMinY = crtc->y;
            const int screenMaxX = crtc->x + static_cast<int> (crtc->width);
            const int screenMaxY = crtc->y + static_cast<int> (crtc->height);

            if (!haveRequestedBounds) {
                minX = screenMinX;
                minY = screenMinY;
                maxX = screenMaxX;
                maxY = screenMaxY;
                haveRequestedBounds = true;
            } else {
                minX = std::min (minX, screenMinX);
                minY = std::min (minY, screenMinY);
                maxX = std::max (maxX, screenMaxX);
                maxY = std::max (maxY, screenMaxY);
            }
        }

        XRRFreeCrtcInfo (crtc);
    }

    XRRFreeScreenResources (screenResources);

    bool any = false;

    for (const auto& o : this->m_screens) {
        const auto cur = this->m_context.settings.general.screenBackgrounds.find (o->name);

        if (cur == this->m_context.settings.general.screenBackgrounds.end ())
            continue;

        any = true;
    }

    if (!any) {
        sLog.error ("No outputs could be initialized, please check the parameters and try again");
        sLog.error ("Detected outputs:");

        for (const auto& o : this->m_screens) {
            sLog.error ("  ", o->name);
        }

        sLog.error ("Requested: ");

        for (const auto& o : this->m_context.settings.general.screenBackgrounds | std::views::keys) {
            sLog.error ("  ", o);
        }

        sLog.exception ("Cannot continue...");
    }

    if (haveRequestedBounds) {
        this->m_rootOffsetX = minX;
        this->m_rootOffsetY = minY;
        this->m_fullWidth = maxX - minX;
        this->m_fullHeight = maxY - minY;

        for (const auto& [name, viewport] : this->m_viewports) {
            viewport->viewport.x -= this->m_rootOffsetX;
            viewport->viewport.y -= this->m_rootOffsetY;
        }

        sLog.out ("X11 render bounds: ", this->m_fullWidth, "x", this->m_fullHeight, " @ ",
                  this->m_rootOffsetX, "x", this->m_rootOffsetY);
    } else {
        this->m_fullWidth = this->m_rootWidth;
        this->m_fullHeight = this->m_rootHeight;
        this->m_rootOffsetX = 0;
        this->m_rootOffsetY = 0;
    }
#ifdef HAVE_XFIXES
    bool xfixesAvailable = false;
    {
        int eventBase = 0;
        int errorBase = 0;
        int major = 0;
        int minor = 0;

        if (XFixesQueryExtension (this->m_display, &eventBase, &errorBase) &&
            XFixesQueryVersion (this->m_display, &major, &minor)) {
            if (major >= 2) {
                xfixesAvailable = true;
            } else {
                sLog.out ("X11 XFixes version too old (", major, ".", minor, "), falling back to root pixmap");
            }
        }
    }

    this->m_usePerOutputWindows = xfixesAvailable;

    if (!xfixesAvailable)
        sLog.out ("X11 XFixes unavailable at runtime, falling back to root pixmap");
#else
    this->m_usePerOutputWindows = false;
#endif

    // create pixmap so we can draw things in there
    this->m_pixmap = XCreatePixmap (this->m_display, this->m_root, this->m_rootWidth, this->m_rootHeight, 24);
    this->m_gc = XCreateGC (this->m_display, this->m_pixmap, 0, nullptr);

    // try to preserve the existing root pixmap (keeps other monitors unchanged)
    Pixmap rootPixmap = None;
    {
        const Atom propRoot = XInternAtom (this->m_display, "_XROOTPMAP_ID", False);
        const Atom propEsetroot = XInternAtom (this->m_display, "ESETROOT_PMAP_ID", False);

        auto resolveRootPixmap = [&](Atom prop) -> Pixmap {
            Atom actualType = None;
            int actualFormat = 0;
            unsigned long nitems = 0;
            unsigned long bytesAfter = 0;
            unsigned char* data = nullptr;

            if (XGetWindowProperty (this->m_display, this->m_root, prop, 0, 1, False, XA_PIXMAP,
                                    &actualType, &actualFormat, &nitems, &bytesAfter, &data) == Success &&
                actualType == XA_PIXMAP && nitems == 1 && data != nullptr) {
                const auto result = *reinterpret_cast<Pixmap*> (data);
                XFree (data);
                return result;
            }

            if (data != nullptr)
                XFree (data);

            return None;
        };

        rootPixmap = resolveRootPixmap (propRoot);

        if (rootPixmap == None)
            rootPixmap = resolveRootPixmap (propEsetroot);
    }

    if (rootPixmap != None) {
        Window pixRoot = 0;
        int pixX = 0;
        int pixY = 0;
        unsigned int pixW = 0;
        unsigned int pixH = 0;
        unsigned int pixBorder = 0;
        unsigned int pixDepth = 0;

        if (XGetGeometry (this->m_display, rootPixmap, &pixRoot, &pixX, &pixY, &pixW, &pixH, &pixBorder, &pixDepth) &&
            pixDepth == static_cast<unsigned int> (DefaultDepth (this->m_display, DefaultScreen (this->m_display)))) {
            const unsigned int copyW = std::min (pixW, static_cast<unsigned int> (this->m_rootWidth));
            const unsigned int copyH = std::min (pixH, static_cast<unsigned int> (this->m_rootHeight));
            if (copyW != static_cast<unsigned int> (this->m_rootWidth) ||
                copyH != static_cast<unsigned int> (this->m_rootHeight)) {
                XFillRectangle (this->m_display, this->m_pixmap, this->m_gc, 0, 0, this->m_rootWidth, this->m_rootHeight);
            }

            XCopyArea (this->m_display, rootPixmap, this->m_pixmap, this->m_gc, 0, 0, copyW, copyH, 0, 0);
            sLog.out ("X11 preserved root pixmap");
        } else {
            XFillRectangle (this->m_display, this->m_pixmap, this->m_gc, 0, 0, this->m_rootWidth, this->m_rootHeight);
            sLog.out ("X11 root pixmap incompatible, filled black");
        }
    } else {
        // pre-fill it with black if no root pixmap is available
        XFillRectangle (this->m_display, this->m_pixmap, this->m_gc, 0, 0, this->m_rootWidth, this->m_rootHeight);
        sLog.out ("X11 root pixmap missing, filled black");
    }
    // set the window background as our pixmap
    XSetWindowBackgroundPixmap (this->m_display, this->m_root, this->m_pixmap);
    // expose the pixmap for other programs/compositors (set once, not per-frame)
    const Atom prop_root = XInternAtom (this->m_display, "_XROOTPMAP_ID", False);
    const Atom prop_esetroot = XInternAtom (this->m_display, "ESETROOT_PMAP_ID", False);
    XChangeProperty (this->m_display, this->m_root, prop_root, XA_PIXMAP, 32, PropModeReplace,
                     (unsigned char*) &this->m_pixmap, 1);
    XChangeProperty (this->m_display, this->m_root, prop_esetroot, XA_PIXMAP, 32, PropModeReplace,
                     (unsigned char*) &this->m_pixmap, 1);

    if (this->m_usePerOutputWindows) {
        sLog.out ("X11 per-output windows enabled");

        const Atom netWmWindowType = XInternAtom (this->m_display, "_NET_WM_WINDOW_TYPE", False);
        const Atom netWmWindowTypeDesktop = XInternAtom (this->m_display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
        const Atom netWmState = XInternAtom (this->m_display, "_NET_WM_STATE", False);
        const Atom netWmStateBelow = XInternAtom (this->m_display, "_NET_WM_STATE_BELOW", False);
        const Atom netWmStateSticky = XInternAtom (this->m_display, "_NET_WM_STATE_STICKY", False);
        const Atom netWmStateSkipTaskbar = XInternAtom (this->m_display, "_NET_WM_STATE_SKIP_TASKBAR", False);
        const Atom netWmStateSkipPager = XInternAtom (this->m_display, "_NET_WM_STATE_SKIP_PAGER", False);

        for (const auto& [name, viewport] : this->m_viewports) {
            const int absX = viewport->viewport.x + this->m_rootOffsetX;
            const int absY = viewport->viewport.y + this->m_rootOffsetY;
            const unsigned int width = static_cast<unsigned int> (viewport->viewport.z);
            const unsigned int height = static_cast<unsigned int> (viewport->viewport.w);

            XSetWindowAttributes attributes;
            attributes.override_redirect = True;
            attributes.background_pixmap = None;

            const Window window = XCreateWindow (this->m_display,
                                                 this->m_root,
                                                 absX,
                                                 absY,
                                                 width,
                                                 height,
                                                 0,
                                                 CopyFromParent,
                                                 InputOutput,
                                                 CopyFromParent,
                                                 CWOverrideRedirect | CWBackPixmap,
                                                 &attributes);

            XChangeProperty (this->m_display, window, netWmWindowType, XA_ATOM, 32, PropModeReplace,
                             reinterpret_cast<unsigned char*> (const_cast<Atom*> (&netWmWindowTypeDesktop)), 1);

            const Atom states[] = {netWmStateBelow, netWmStateSticky, netWmStateSkipTaskbar, netWmStateSkipPager};
            XChangeProperty (this->m_display, window, netWmState, XA_ATOM, 32, PropModeReplace,
                             reinterpret_cast<unsigned char*> (const_cast<Atom*> (states)), 4);

#ifdef HAVE_XFIXES
            XserverRegion region = XFixesCreateRegion (this->m_display, nullptr, 0);
            XFixesSetWindowShapeRegion (this->m_display, window, ShapeInput, 0, 0, region);
            XFixesDestroyRegion (this->m_display, region);
#endif

            XMapWindow (this->m_display, window);
            XLowerWindow (this->m_display, window);

            this->m_windows [name] = window;
            this->m_windowGCs [name] = XCreateGC (this->m_display, window, 0, nullptr);
        }

        XFlush (this->m_display);
    }
    // allocate space for the image's data
    this->m_imageSize = this->m_fullWidth * this->m_fullHeight * 4;
    this->m_imageData = new char [this->m_fullWidth * this->m_fullHeight * 4];
    // create an image so we can copy it over
    this->m_image = XCreateImage (this->m_display, CopyFromParent, 24, ZPixmap, 0, this->m_imageData, this->m_fullWidth,
                                  this->m_fullHeight, 32, 0);
    // setup driver's render changing the window's size
    if (auto* glfwDriver = dynamic_cast<WallpaperEngine::Render::Drivers::GLFWOpenGLDriver*> (&this->m_driver)) {
        glfwDriver->ensureFramebufferSize ({this->m_fullWidth, this->m_fullHeight});
    } else {
        this->m_driver.resizeWindow ({this->m_fullWidth, this->m_fullHeight});
    }
}

void X11Output::updateRender () const {
    if (this->m_usePerOutputWindows) {
        for (const auto& [name, viewport] : this->m_viewports) {
            const auto windowIt = this->m_windows.find (name);
            const auto gcIt = this->m_windowGCs.find (name);

            if (windowIt == this->m_windows.end () || gcIt == this->m_windowGCs.end ())
                continue;

            XPutImage (this->m_display,
                       windowIt->second,
                       gcIt->second,
                       this->m_image,
                       viewport->viewport.x,
                       viewport->viewport.y,
                       0,
                       0,
                       viewport->viewport.z,
                       viewport->viewport.w);
        }

        // keep root pixmap and properties updated for pseudo-transparency consumers
        if (this->m_pixmap != None && this->m_gc != None) {
            XPutImage (this->m_display,
                       this->m_pixmap,
                       this->m_gc,
                       this->m_image,
                       0,
                       0,
                       this->m_rootOffsetX,
                       this->m_rootOffsetY,
                       this->m_fullWidth,
                       this->m_fullHeight);

            const Atom prop_root = XInternAtom (this->m_display, "_XROOTPMAP_ID", False);
            const Atom prop_esetroot = XInternAtom (this->m_display, "ESETROOT_PMAP_ID", False);
            XChangeProperty (this->m_display, this->m_root, prop_root, XA_PIXMAP, 32, PropModeReplace,
                             (unsigned char*) &this->m_pixmap, 1);
            XChangeProperty (this->m_display, this->m_root, prop_esetroot, XA_PIXMAP, 32, PropModeReplace,
                             (unsigned char*) &this->m_pixmap, 1);
        }

        XFlush (this->m_display);
        return;
    }

    // put the image back into the screen
    XPutImage (this->m_display,
               this->m_pixmap,
               this->m_gc,
               this->m_image,
               0,
               0,
               this->m_rootOffsetX,
               this->m_rootOffsetY,
               this->m_fullWidth,
               this->m_fullHeight);

    // some compositors only refresh the background when these properties update
    const Atom prop_root = XInternAtom (this->m_display, "_XROOTPMAP_ID", False);
    const Atom prop_esetroot = XInternAtom (this->m_display, "ESETROOT_PMAP_ID", False);
    XChangeProperty (this->m_display, this->m_root, prop_root, XA_PIXMAP, 32, PropModeReplace,
                     (unsigned char*) &this->m_pixmap, 1);
    XChangeProperty (this->m_display, this->m_root, prop_esetroot, XA_PIXMAP, 32, PropModeReplace,
                     (unsigned char*) &this->m_pixmap, 1);

    // only mark the target region dirty to avoid forcing full-root redraws
    XClearArea (this->m_display,
                this->m_root,
                this->m_rootOffsetX,
                this->m_rootOffsetY,
                static_cast<unsigned int> (this->m_fullWidth),
                static_cast<unsigned int> (this->m_fullHeight),
                False);
    XFlush (this->m_display);
}
