//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "macos_frameless_window.hpp"

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <QQuickWindow>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <cmath>

#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui {

// Matches DESIGN.md: close-button inset that lines the lights up with the
// collections wordmark after the default system leading margin.
constexpr CGFloat kTrafficLightHorizontalOffset = 17.0;
constexpr CGFloat kPositionEpsilon              = 0.25;
constexpr CGFloat kTitlebarBottomPad            = 4.0;

char kChromeMarkKey;
char kControllerKey;
char kHitTestSwizzledKey;

auto ToolbarCenterFromTop() -> CGFloat {
  const auto& theme          = AppTheme::Instance();
  const auto  toolbar_height = theme.iconButtonHitSizeCompact() + theme.spaceSm();
  return theme.spaceMd() + static_cast<CGFloat>(toolbar_height) / 2.0;
}

auto IsAlcedoChromeWindow(NSWindow* window) -> bool {
  return window && objc_getAssociatedObject(window, &kChromeMarkKey) != nil;
}

auto MarkAlcedoChromeWindow(NSWindow* window) -> void {
  if (!window) {
    return;
  }
  objc_setAssociatedObject(window, &kChromeMarkKey, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

auto IsStandardTrafficLight(NSView* view, NSWindow* window) -> bool {
  if (!view || !window) {
    return false;
  }
  for (NSView* current = view; current != nil; current = current.superview) {
    if (current == [window standardWindowButton:NSWindowCloseButton] ||
        current == [window standardWindowButton:NSWindowMiniaturizeButton] ||
        current == [window standardWindowButton:NSWindowZoomButton]) {
      return true;
    }
  }
  return false;
}

auto TitlebarContainer(NSWindow* window) -> NSView* {
  NSButton* close = [window standardWindowButton:NSWindowCloseButton];
  if (!close.superview) {
    return nil;
  }
  return close.superview.superview;
}

auto FindDescendantByClassName(NSView* root, NSString* class_name) -> NSView* {
  if (!root || class_name.length == 0) {
    return nil;
  }
  if ([root.className isEqualToString:class_name]) {
    return root;
  }
  for (NSView* child in root.subviews) {
    if (NSView* found = FindDescendantByClassName(child, class_name)) {
      return found;
    }
  }
  return nil;
}

auto RestoreButtonsToTitlebar(NSWindow* window) -> void {
  NSView* frame_view = window.contentView.superview;
  if (!frame_view) {
    return;
  }
  NSView* titlebar_view = FindDescendantByClassName(frame_view, @"NSTitlebarView");
  if (!titlebar_view) {
    return;
  }
  const NSWindowButton kinds[] = {NSWindowCloseButton, NSWindowMiniaturizeButton,
                                  NSWindowZoomButton};
  for (const NSWindowButton kind : kinds) {
    NSButton* button = [window standardWindowButton:kind];
    if (button && button.superview != titlebar_view) {
      [titlebar_view addSubview:button];
    }
  }
}

using HitTestFn = NSView* (*)(id, SEL, NSPoint);

auto OriginalHitTestFn(Class cls) -> HitTestFn {
  for (Class current = cls; current != nil; current = class_getSuperclass(current)) {
    NSValue* stored = objc_getAssociatedObject(current, &kHitTestSwizzledKey);
    if (stored) {
      return reinterpret_cast<HitTestFn>(stored.pointerValue);
    }
  }
  return nullptr;
}

auto SwizzledTitlebarHitTest(id self, SEL selector, NSPoint point) -> NSView* {
  HitTestFn impl = OriginalHitTestFn(object_getClass(self));
  NSView*   hit  = impl ? impl(self, selector, point) : nil;
  NSWindow* window   = [self window];
  if (!IsAlcedoChromeWindow(window)) {
    return hit;
  }
  if (IsStandardTrafficLight(hit, window)) {
    return hit;
  }
  // Empty title-bar chrome must not steal the QML toolbar. Window dragging is
  // owned by TopToolbar's DragHandler once the event reaches the scene.
  return nil;
}

auto SwizzleTitlebarHitTest(Class cls) -> void {
  if (!cls || objc_getAssociatedObject(cls, &kHitTestSwizzledKey)) {
    return;
  }
  const SEL selector = @selector(hitTest:);
  const Method method = class_getInstanceMethod(cls, selector);
  if (!method) {
    return;
  }
  HitTestFn original = reinterpret_cast<HitTestFn>(method_getImplementation(method));
  objc_setAssociatedObject(cls, &kHitTestSwizzledKey,
                           [NSValue valueWithPointer:reinterpret_cast<void*>(original)],
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  class_replaceMethod(cls, selector, reinterpret_cast<IMP>(SwizzledTitlebarHitTest),
                      method_getTypeEncoding(method));
}

auto InstallTitlebarClickThrough(NSWindow* window) -> void {
  NSView* container = TitlebarContainer(window);
  if (!container) {
    return;
  }
  SwizzleTitlebarHitTest([container class]);
  if (NSView* titlebar = container.subviews.firstObject) {
    SwizzleTitlebarHitTest([titlebar class]);
  }
  SwizzleTitlebarHitTest(NSClassFromString(@"NSTitlebarContainerView"));
  SwizzleTitlebarHitTest(NSClassFromString(@"NSTitlebarView"));
}

}  // namespace alcedo::ui

@interface AlcedoTrafficLightController : NSObject
- (instancetype)initWithWindow:(NSWindow*)window;
- (void)align;
- (void)scheduleAlign;
@end

@implementation AlcedoTrafficLightController {
  NSWindow* _window;
  NSView*   _watchedHost;
  CGFloat   _baseCloseX;
  CGFloat   _buttonPadding;
  BOOL      _metricsCaptured;
  BOOL      _aligning;
}

- (instancetype)initWithWindow:(NSWindow*)window {
  self = [super init];
  if (!self) {
    return nil;
  }
  _window          = window;
  _watchedHost     = nil;
  _baseCloseX      = 0.0;
  _buttonPadding   = 6.0;
  _metricsCaptured = NO;
  _aligning        = NO;

  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  [center addObserver:self
             selector:@selector(scheduleAlign)
                 name:NSWindowDidResizeNotification
               object:window];
  [center addObserver:self
             selector:@selector(scheduleAlign)
                 name:NSWindowDidDeminiaturizeNotification
               object:window];
  [center addObserver:self
             selector:@selector(scheduleAlign)
                 name:NSWindowDidEndLiveResizeNotification
               object:window];
  [center addObserver:self
             selector:@selector(scheduleAlign)
                 name:NSWindowDidExitFullScreenNotification
               object:window];
  return self;
}

- (void)dealloc {
  [NSObject cancelPreviousPerformRequestsWithTarget:self];
  [NSNotificationCenter.defaultCenter removeObserver:self];
#if !__has_feature(objc_arc)
  [super dealloc];
#endif
}

- (void)scheduleAlign {
  [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(align) object:nil];
  [self performSelector:@selector(align) withObject:nil afterDelay:0];
  [self performSelector:@selector(align) withObject:nil afterDelay:0.016];
}

- (void)watchHost:(NSView*)host {
  if (host == _watchedHost) {
    return;
  }
  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  if (_watchedHost) {
    [center removeObserver:self
                      name:NSViewFrameDidChangeNotification
                    object:_watchedHost];
  }
  _watchedHost = host;
  if (!host) {
    return;
  }
  host.postsFrameChangedNotifications = YES;
  [center addObserver:self
             selector:@selector(hostFrameChanged:)
                 name:NSViewFrameDidChangeNotification
               object:host];
}

- (void)hostFrameChanged:(NSNotification*)notification {
  (void)notification;
  if (_aligning) {
    return;
  }
  [self scheduleAlign];
}

- (void)captureMetricsIfNeeded {
  if (_metricsCaptured) {
    return;
  }
  NSButton* close = [_window standardWindowButton:NSWindowCloseButton];
  NSButton* mini  = [_window standardWindowButton:NSWindowMiniaturizeButton];
  if (!close || !mini) {
    return;
  }
  _baseCloseX      = NSMinX(close.frame);
  _buttonPadding   = NSMinX(mini.frame) - NSMaxX(close.frame);
  if (_buttonPadding < 1.0) {
    _buttonPadding = 6.0;
  }
  _metricsCaptured = YES;
}

- (void)align {
  if (!_window || _aligning) {
    return;
  }
  if (_window.styleMask & NSWindowStyleMaskFullScreen) {
    return;
  }

  alcedo::ui::RestoreButtonsToTitlebar(_window);
  alcedo::ui::InstallTitlebarClickThrough(_window);

  NSButton* close = [_window standardWindowButton:NSWindowCloseButton];
  NSButton* mini  = [_window standardWindowButton:NSWindowMiniaturizeButton];
  NSButton* zoom  = [_window standardWindowButton:NSWindowZoomButton];
  NSView*   host  = alcedo::ui::TitlebarContainer(_window);
  if (!close || !mini || !zoom || !host) {
    return;
  }

  _aligning = YES;
  [self watchHost:host];
  [self captureMetricsIfNeeded];

  const CGFloat button_height    = NSHeight(close.frame);
  const CGFloat button_width     = NSWidth(close.frame);
  const CGFloat center_from_top  = alcedo::ui::ToolbarCenterFromTop();
  const CGFloat top_margin       = std::max(CGFloat(0.0), center_from_top - button_height / 2.0);
  const CGFloat container_height = top_margin + button_height + alcedo::ui::kTitlebarBottomPad;
  const CGFloat start_x          = _baseCloseX + alcedo::ui::kTrafficLightHorizontalOffset;
  const CGFloat origin_y         = alcedo::ui::kTitlebarBottomPad;

  NSRect container_frame      = host.frame;
  container_frame.size.height = container_height;
  container_frame.origin.y    = NSHeight(_window.frame) - container_height;

  const bool container_moved =
      std::abs(NSMinY(host.frame) - NSMinY(container_frame)) >= alcedo::ui::kPositionEpsilon ||
      std::abs(NSHeight(host.frame) - container_height) >= alcedo::ui::kPositionEpsilon;
  if (container_moved) {
    host.postsFrameChangedNotifications = YES;
    host.frame                          = container_frame;
  }

  const NSPoint origins[] = {
      NSMakePoint(start_x, origin_y),
      NSMakePoint(start_x + button_width + _buttonPadding, origin_y),
      NSMakePoint(start_x + 2.0 * (button_width + _buttonPadding), origin_y),
  };
  NSButton* buttons[] = {close, mini, zoom};
  for (size_t index = 0; index < 3; ++index) {
    NSButton* button = buttons[index];
    button.hidden    = NO;
    button.translatesAutoresizingMaskIntoConstraints = YES;
    if (std::abs(NSMinX(button.frame) - origins[index].x) < alcedo::ui::kPositionEpsilon &&
        std::abs(NSMinY(button.frame) - origins[index].y) < alcedo::ui::kPositionEpsilon) {
      continue;
    }
    button.postsFrameChangedNotifications = YES;
    [button setFrameOrigin:origins[index]];
    [button updateTrackingAreas];
    [button.window invalidateCursorRectsForView:button];
  }
  [host updateTrackingAreas];
  _aligning = NO;
}

@end

namespace alcedo::ui {
namespace {

auto ControllerFor(NSWindow* window) -> AlcedoTrafficLightController* {
  if (!window) {
    return nil;
  }
  auto* controller = static_cast<AlcedoTrafficLightController*>(
      objc_getAssociatedObject(window, &kControllerKey));
  if (!controller) {
    controller = [[AlcedoTrafficLightController alloc] initWithWindow:window];
    objc_setAssociatedObject(window, &kControllerKey, controller,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
  return controller;
}

}  // namespace

auto MacosFramelessWindow::Apply(QQuickWindow* window) -> bool {
  if (!window) {
    return false;
  }

  const auto native_handle = reinterpret_cast<void*>(window->winId());
  if (!native_handle) {
    return false;
  }

  id        object    = (__bridge id)native_handle;
  NSWindow* ns_window = nil;
  if ([object isKindOfClass:[NSWindow class]]) {
    ns_window = (NSWindow*)object;
  } else if ([object isKindOfClass:[NSView class]]) {
    ns_window = ((NSView*)object).window;
  }
  if (!ns_window) {
    return false;
  }

  const NSWindowStyleMask chrome_mask =
      NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
      NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView;
  ns_window.styleMask |= chrome_mask;
  ns_window.titlebarAppearsTransparent = YES;
  ns_window.titleVisibility            = NSWindowTitleHidden;
  if (@available(macOS 11.0, *)) {
    ns_window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
  }

  MarkAlcedoChromeWindow(ns_window);
  AlcedoTrafficLightController* controller = ControllerFor(ns_window);
  [controller align];
  return true;
}

auto MacosFramelessWindow::Install(QQuickWindow* window) -> bool {
  if (installed_ || !window) {
    return false;
  }
  if (!Apply(window)) {
    return false;
  }

  window_            = window;
  const auto realign = [window]() {
    Apply(window);
    QTimer::singleShot(0, window, [window]() { Apply(window); });
  };
  QObject::connect(window, &QWindow::visibilityChanged, window,
                   [realign](QWindow::Visibility) { realign(); });
  QObject::connect(window, &QWindow::windowStateChanged, window,
                   [realign](Qt::WindowState) { realign(); });
  QObject::connect(window, &QWindow::heightChanged, window, realign);
  QObject::connect(window, &QWindow::widthChanged, window, realign);
  installed_ = true;
  return true;
}

}  // namespace alcedo::ui
