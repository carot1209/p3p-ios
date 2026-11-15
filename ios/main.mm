// iOS shell of the P3P static recompilation: a landscape screen with the PSP
// picture, touch buttons, live audio and the save/load list. Everything about
// the game itself lives in runner/live_runner.cpp (portable C++).
//
// Files (Files app -> On My iPhone -> Persona 3 P):
//   <any name>.iso   the user's ULUS-10512 image (the first .iso found is used)
//   ms0/             memory stick (saves), created by the app
//   report.txt       build, speed and the reason of a stop; attach it to bug reports
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include "live_runner.hpp"
#include "metal_backend.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef P3P_BUILD_ID
#define P3P_BUILD_ID "dev"
#endif

static std::atomic<p3pios::LiveRunner *> gAudioRunner{nullptr};

// ---------------------------------------------------------------------------
// Audio: a RemoteIO unit pulling 44.1 kHz stereo S16 from the runner.
// ---------------------------------------------------------------------------
static OSStatus P3PRender(void *, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32 frames, AudioBufferList *data) {
    if (data == nullptr || data->mNumberBuffers < 1 || data->mBuffers[0].mData == nullptr) return noErr;
    auto *out = static_cast<std::int16_t *>(data->mBuffers[0].mData);
    const UInt32 capacity = data->mBuffers[0].mDataByteSize / 4u;
    const UInt32 count = frames < capacity ? frames : capacity;
    p3pios::LiveRunner *runner = gAudioRunner.load(std::memory_order_acquire);
    if (runner != nullptr) runner->pull_audio(out, count);
    else std::memset(out, 0, count * 4u);
    return noErr;
}

@interface P3PAudio : NSObject
- (void)start;
- (void)resume;
@end

// A Persona mod package (Aemulus layout: data/ and FMV/ next to Package.xml,
// e.g. "Cutscenes Restored (Subtitled)") or a folder holding only FMV/NN.pmsf dropped into the app's folder, up
// to two levels deep, is moved where the game looks for it: data/ into
// mods/bind/data/ (Mod Support's loose files) and FMV/*.pmsf into mods/FMV/
// (ms0:/PSP/P3P is the mods/ folder). Returns the number of files moved.
static int install_mod_packages(const std::filesystem::path &documents) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> packages;
    std::vector<fs::path> level{documents};
    for (int depth = 0; depth < 2; ++depth) {
        std::vector<fs::path> next;
        for (const auto &dir : level)
            for (const auto &entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_directory(ec)) continue;
                const std::string name = entry.path().filename().string();
                if (depth == 0 && (name == "mods" || name == "ms0" || name == "movies")) continue;
                const bool described = fs::exists(entry.path() / "Package.xml", ec) &&
                                       (fs::is_directory(entry.path() / "data", ec) || fs::is_directory(entry.path() / "FMV", ec));
                // A movie-only pack (another dub of the same movies) is just FMV/NN.pmsf.
                const bool movies = fs::exists(entry.path() / "FMV" / "00.pmsf", ec);
                if (described) packages.insert(packages.begin(), entry.path()); // full packages first,
                else if (movies) packages.push_back(entry.path());              // movie packs over them
                else next.push_back(entry.path());
            }
        level = next;
    }
    int moved = 0;
    const auto move_tree = [&](const fs::path &from, const fs::path &to) {
        for (auto it = fs::recursive_directory_iterator(from, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const fs::path target = to / fs::relative(it->path(), from, ec);
            fs::create_directories(target.parent_path(), ec);
            fs::remove(target, ec);
            fs::rename(it->path(), target, ec);
            if (!ec) ++moved;
        }
    };
    for (const auto &package : packages) {
        move_tree(package / "data", documents / "mods" / "bind" / "data");
        move_tree(package / "FMV", documents / "mods" / "FMV");
        fs::rename(package, documents / "mods" / ("installed-" + package.filename().string()), ec);
    }
    return moved;
}

@implementation P3PAudio {
    AudioUnit _unit;
    BOOL _ready;
}

- (void)start {
    AVAudioSession *session = [AVAudioSession sharedInstance];
    [session setCategory:AVAudioSessionCategoryPlayback error:nil];
    [session setPreferredSampleRate:44100.0 error:nil];
    [session setPreferredIOBufferDuration:0.012 error:nil];
    [session setActive:YES error:nil];

    AudioComponentDescription description;
    std::memset(&description, 0, sizeof description);
    description.componentType = kAudioUnitType_Output;
    description.componentSubType = kAudioUnitSubType_RemoteIO;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent component = AudioComponentFindNext(NULL, &description);
    if (component == NULL) return;
    if (AudioComponentInstanceNew(component, &_unit) != noErr) return;

    AudioStreamBasicDescription format;
    std::memset(&format, 0, sizeof format);
    format.mSampleRate = 44100.0;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mBitsPerChannel = 16;
    format.mChannelsPerFrame = 2;
    format.mBytesPerFrame = 4;
    format.mFramesPerPacket = 1;
    format.mBytesPerPacket = 4;
    AudioUnitSetProperty(_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof format);

    AURenderCallbackStruct callback;
    callback.inputProc = P3PRender;
    callback.inputProcRefCon = NULL;
    AudioUnitSetProperty(_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof callback);

    if (AudioUnitInitialize(_unit) != noErr) return;
    _ready = YES;
    AudioOutputUnitStart(_unit);
}

// After an interruption (a call, another app's audio) or a return from the background.
- (void)resume {
    if (!_ready) return;
    [[AVAudioSession sharedInstance] setActive:YES error:nil];
    AudioOutputUnitStart(_unit);
}

@end

// ---------------------------------------------------------------------------
// Touch controls: PSP buttons drawn over the picture.
// ---------------------------------------------------------------------------
struct P3PButton {
    CGRect frame;
    std::uint32_t mask;
    const char *label;
    bool round;
};

@interface P3PControlsView : UIView
@property(nonatomic, assign) p3pios::LiveRunner *runner;
@property(nonatomic, assign) BOOL inputEnabled;
@property(nonatomic, assign) BOOL gesturesEnabled;
@end

@implementation P3PControlsView {
    std::vector<P3PButton> _buttons;
    CGPoint _padCenter;
    CGFloat _padRadius;
    std::uint32_t _mask;
    // Gestures on the picture (touches that start away from the buttons):
    // tap = CROSS, two-finger tap = CIRCLE, swipe = a d-pad press.
    NSMapTable<UITouch *, NSValue *> *_gestureStart; // touch -> start point
    NSTimeInterval _gestureBegan;
    NSUInteger _gestureFingers;
    BOOL _gestureMoved;
    std::uint32_t _pulse;
    std::uint64_t _pulseSerial;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.multipleTouchEnabled = YES;
        self.exclusiveTouch = NO;
        self.opaque = NO;
        self.backgroundColor = [UIColor clearColor];
        self.contentMode = UIViewContentModeRedraw;
        _inputEnabled = YES;
        _gesturesEnabled = YES;
        _gestureStart = [NSMapTable weakToStrongObjectsMapTable];
    }
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    using namespace p3pios::button;
    const CGRect b = self.bounds;
    const UIEdgeInsets insets = self.safeAreaInsets;
    const CGFloat left = b.origin.x + MAX(insets.left, 12.0);
    const CGFloat right = CGRectGetMaxX(b) - MAX(insets.right, 12.0);
    const CGFloat bottom = CGRectGetMaxY(b) - MAX(insets.bottom, 10.0);
    const CGFloat top = b.origin.y + MAX(insets.top, 8.0);
    const CGFloat unit = MIN(b.size.height, b.size.width) / 414.0; // sizes for a 414 pt high screen

    _padRadius = 72.0 * unit;
    _padCenter = CGPointMake(left + _padRadius + 6.0 * unit, bottom - _padRadius - 34.0 * unit);

    _buttons.clear();
    const CGFloat face = 58.0 * unit, spread = 56.0 * unit;
    const CGPoint fc = CGPointMake(right - spread - face / 2.0 - 6.0 * unit, bottom - spread - face / 2.0 - 34.0 * unit);
    auto addRound = [&](CGFloat cx, CGFloat cy, std::uint32_t mask, const char *label) {
        _buttons.push_back({CGRectMake(cx - face / 2.0, cy - face / 2.0, face, face), mask, label, true});
    };
    addRound(fc.x, fc.y - spread, Triangle, "\xE2\x96\xB3");          // top
    addRound(fc.x + spread, fc.y, Circle, "\xE2\x97\x8B");            // right
    addRound(fc.x, fc.y + spread, Cross, "\xE2\x9C\x95");             // bottom
    addRound(fc.x - spread, fc.y, Square, "\xE2\x96\xA1");            // left

    const CGFloat shoulderW = 96.0 * unit, shoulderH = 40.0 * unit;
    _buttons.push_back({CGRectMake(left, top, shoulderW, shoulderH), L, "L", false});
    _buttons.push_back({CGRectMake(right - shoulderW, top, shoulderW, shoulderH), R, "R", false});

    // SELECT under L and START under R: the bottom middle is where the
    // subtitles and the message window are.
    const CGFloat smallW = 76.0 * unit, smallH = 28.0 * unit, gap = 14.0 * unit;
    _buttons.push_back({CGRectMake(left, top + shoulderH + gap, smallW, smallH), Select, "SELECT", false});
    _buttons.push_back({CGRectMake(right - smallW, top + shoulderH + gap, smallW, smallH), Start, "START", false});
    [self setNeedsDisplay];
}

- (std::uint32_t)maskAtPoint:(CGPoint)p {
    using namespace p3pios::button;
    std::uint32_t mask = 0;
    for (const auto &button : _buttons)
        if (CGRectContainsPoint(CGRectInset(button.frame, -10.0, -10.0), p)) mask |= button.mask;
    const CGFloat dx = p.x - _padCenter.x, dy = p.y - _padCenter.y;
    const CGFloat distance = std::sqrt(dx * dx + dy * dy);
    if (distance <= _padRadius * 1.35 && distance >= _padRadius * 0.18) {
        const CGFloat threshold = distance * 0.38; // 8 directions
        if (dx < -threshold) mask |= Left;
        if (dx > threshold) mask |= Right;
        if (dy < -threshold) mask |= Up;
        if (dy > threshold) mask |= Down;
    }
    return mask;
}

- (void)sendPad {
    if (_runner != nullptr) _runner->set_pad(_inputEnabled ? (_mask | _pulse) : 0u);
}

- (void)updateWithEvent:(UIEvent *)event {
    std::uint32_t mask = 0;
    for (UITouch *touch in [event touchesForView:self]) {
        if (touch.phase == UITouchPhaseEnded || touch.phase == UITouchPhaseCancelled) continue;
        if ([_gestureStart objectForKey:touch] != nil) continue; // a gesture, not a button
        mask |= [self maskAtPoint:[touch locationInView:self]];
    }
    if (!_inputEnabled) mask = 0;
    if (mask != _mask) {
        _mask = mask;
        [self sendPad];
        [self setNeedsDisplay];
    }
}

// A short press the game sees for a few of its frames (it reads the pad at
// 30 or 60 Hz), then a release.
- (void)pulse:(std::uint32_t)buttons {
    if (!_inputEnabled || buttons == 0u) return;
    _pulse = buttons;
    const std::uint64_t serial = ++_pulseSerial;
    [self sendPad];
    __weak P3PControlsView *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(120 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
        P3PControlsView *strong = weakSelf;
        if (strong == nil || strong->_pulseSerial != serial) return;
        strong->_pulse = 0u;
        [strong sendPad];
    });
}

- (void)gestureBegan:(NSSet<UITouch *> *)touches {
    if (!_gesturesEnabled || !_inputEnabled) return;
    for (UITouch *touch in touches) {
        const CGPoint p = [touch locationInView:self];
        if ([self maskAtPoint:p] != 0u) continue;
        if (_gestureStart.count == 0) {
            _gestureBegan = touch.timestamp;
            _gestureFingers = 0;
            _gestureMoved = NO;
        }
        [_gestureStart setObject:[NSValue valueWithCGPoint:p] forKey:touch];
        _gestureFingers = MAX(_gestureFingers, (NSUInteger)_gestureStart.count);
    }
}

- (void)gestureEnded:(NSSet<UITouch *> *)touches cancelled:(BOOL)cancelled {
    using namespace p3pios::button;
    for (UITouch *touch in touches) {
        NSValue *start = [_gestureStart objectForKey:touch];
        if (start == nil) continue;
        [_gestureStart removeObjectForKey:touch];
        const CGPoint a = start.CGPointValue, b = [touch locationInView:self];
        const CGFloat dx = b.x - a.x, dy = b.y - a.y, distance = std::sqrt(dx * dx + dy * dy);
        const NSTimeInterval held = touch.timestamp - _gestureBegan;
        if (cancelled || _gestureMoved) continue;
        if (distance >= 40.0 && held < 0.6 && _gestureFingers == 1) {
            // Swipe: one d-pad step in its main direction.
            _gestureMoved = YES;
            if (std::fabs(dx) > std::fabs(dy)) [self pulse:dx < 0 ? Left : Right];
            else [self pulse:dy < 0 ? Up : Down];
        } else if (distance < 16.0 && held < 0.35 && _gestureStart.count == 0) {
            [self pulse:_gestureFingers >= 2 ? Circle : Cross];
        } else if (distance >= 16.0) {
            _gestureMoved = YES;
        }
    }
}

- (void)releaseAll {
    _mask = 0;
    _pulse = 0;
    ++_pulseSerial;
    [_gestureStart removeAllObjects];
    if (_runner != nullptr) _runner->set_pad(0);
    [self setNeedsDisplay];
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self gestureBegan:touches];
    [self updateWithEvent:event];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self updateWithEvent:event]; }
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self gestureEnded:touches cancelled:NO];
    [self updateWithEvent:event];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self gestureEnded:touches cancelled:YES];
    [self updateWithEvent:event];
}

- (void)drawRect:(CGRect)rect {
    using namespace p3pios::button;
    CGContextRef context = UIGraphicsGetCurrentContext();
    if (context == NULL) return;
    UIColor *idle = [UIColor colorWithWhite:1.0 alpha:0.16];
    UIColor *held = [UIColor colorWithWhite:1.0 alpha:0.42];
    UIColor *line = [UIColor colorWithWhite:1.0 alpha:0.45];

    // D-pad: a disc with four arrows.
    const CGRect disc = CGRectMake(_padCenter.x - _padRadius, _padCenter.y - _padRadius, _padRadius * 2.0, _padRadius * 2.0);
    CGContextSetFillColorWithColor(context, idle.CGColor);
    CGContextFillEllipseInRect(context, disc);
    CGContextSetStrokeColorWithColor(context, line.CGColor);
    CGContextSetLineWidth(context, 1.5);
    CGContextStrokeEllipseInRect(context, CGRectInset(disc, 1.0, 1.0));
    struct Arrow { std::uint32_t mask; CGFloat dx, dy; };
    const Arrow arrows[] = {{Up, 0, -1}, {Down, 0, 1}, {Left, -1, 0}, {Right, 1, 0}};
    for (const Arrow &arrow : arrows) {
        const CGFloat cx = _padCenter.x + arrow.dx * _padRadius * 0.58, cy = _padCenter.y + arrow.dy * _padRadius * 0.58;
        const CGFloat s = _padRadius * 0.24;
        CGContextSetFillColorWithColor(context, ((_mask & arrow.mask) ? held : line).CGColor);
        CGContextBeginPath(context);
        CGContextMoveToPoint(context, cx + arrow.dx * s, cy + arrow.dy * s);
        CGContextAddLineToPoint(context, cx - arrow.dx * s * 0.6 + arrow.dy * s, cy - arrow.dy * s * 0.6 + arrow.dx * s);
        CGContextAddLineToPoint(context, cx - arrow.dx * s * 0.6 - arrow.dy * s, cy - arrow.dy * s * 0.6 - arrow.dx * s);
        CGContextClosePath(context);
        CGContextFillPath(context);
    }

    for (const auto &button : _buttons) {
        const bool down = (_mask & button.mask) != 0;
        CGContextSetFillColorWithColor(context, (down ? held : idle).CGColor);
        CGContextSetStrokeColorWithColor(context, line.CGColor);
        UIBezierPath *path = button.round ? [UIBezierPath bezierPathWithOvalInRect:button.frame]
                                          : [UIBezierPath bezierPathWithRoundedRect:button.frame cornerRadius:button.frame.size.height * 0.4];
        [path fill];
        path.lineWidth = 1.5;
        [path stroke];
        NSString *text = [NSString stringWithUTF8String:button.label];
        const CGFloat size = button.round ? button.frame.size.height * 0.46 : button.frame.size.height * 0.44;
        NSDictionary *attributes = @{NSFontAttributeName : [UIFont boldSystemFontOfSize:size],
                                     NSForegroundColorAttributeName : [UIColor colorWithWhite:1.0 alpha:0.8]};
        const CGSize textSize = [text sizeWithAttributes:attributes];
        [text drawAtPoint:CGPointMake(CGRectGetMidX(button.frame) - textSize.width / 2.0, CGRectGetMidY(button.frame) - textSize.height / 2.0)
           withAttributes:attributes];
    }
}

@end

// ---------------------------------------------------------------------------
// The game screen.
// ---------------------------------------------------------------------------
@interface P3PViewController : UIViewController
- (void)applicationActive:(BOOL)active;
@end

// A view whose layer the GPU back end draws into (Backend::present).
@interface P3PMetalView : UIView
@end
@implementation P3PMetalView
+ (Class)layerClass { return [CAMetalLayer class]; }
@end

@implementation P3PViewController {
    P3PMetalView *_metalView;
    std::unique_ptr<p3pios::LiveRunner> _runner;
    UIView *_screen;
    P3PControlsView *_controls;
    UILabel *_message;
    UILabel *_statusLabel;
    P3PAudio *_audio;
    CADisplayLink *_link;
    std::vector<std::uint8_t> _frame;
    std::uint64_t _serial;
    std::uint64_t _ticks;
    std::uint64_t _dialogSerial;
    BOOL _started;
    BOOL _reportedEnd;
    BOOL _usesGpuDisplay;
    CGColorSpaceRef _colorSpace;
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CAMetalLayer *layer = (CAMetalLayer *)_metalView.layer;
    const CGFloat factor = _metalView.contentScaleFactor;
    layer.drawableSize = CGSizeMake(_metalView.bounds.size.width * factor, _metalView.bounds.size.height * factor);
}

- (NSString *)documentsPath {
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];
    _frame.assign(p3pios::kScreenWidth * p3pios::kScreenHeight * 4u, 0);
    _colorSpace = CGColorSpaceCreateDeviceRGB();

    _screen = [[UIView alloc] initWithFrame:self.view.bounds];
    _screen.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _screen.backgroundColor = [UIColor blackColor];
    _screen.userInteractionEnabled = NO;
    _screen.layer.contentsGravity = kCAGravityResizeAspect;
    _screen.layer.magnificationFilter = kCAFilterLinear;
    [self.view addSubview:_screen];

    _metalView = [[P3PMetalView alloc] initWithFrame:self.view.bounds];
    _metalView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _metalView.backgroundColor = [UIColor blackColor];
    _metalView.userInteractionEnabled = NO;
    _metalView.hidden = YES;
    [self.view addSubview:_metalView];

    _controls = [[P3PControlsView alloc] initWithFrame:self.view.bounds];
    _controls.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:_controls];

    _message = [[UILabel alloc] initWithFrame:CGRectInset(self.view.bounds, 60.0, 40.0)];
    _message.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _message.numberOfLines = 0;
    _message.textAlignment = NSTextAlignmentCenter;
    _message.textColor = [UIColor whiteColor];
    _message.font = [UIFont systemFontOfSize:15.0];
    _message.userInteractionEnabled = NO;
    _message.hidden = YES;
    [self.view addSubview:_message];

    _statusLabel = [[UILabel alloc] initWithFrame:CGRectMake(0.0, 2.0, self.view.bounds.size.width, 14.0)];
    _statusLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    _statusLabel.textAlignment = NSTextAlignmentCenter;
    _statusLabel.textColor = [UIColor colorWithWhite:1.0 alpha:0.55];
    _statusLabel.font = [UIFont monospacedDigitSystemFontOfSize:10.0 weight:UIFontWeightRegular];
    _statusLabel.userInteractionEnabled = NO;
    [self.view addSubview:_statusLabel];

    // The Files app shows the app's folder once it holds something.
    NSString *note = [[self documentsPath] stringByAppendingPathComponent:@"HUONG-DAN.txt"];
    NSString *text = @"Chep file ISO Persona 3 Portable (ULUS-10512) cua ban vao thu muc nay, roi mo lai app.\n"
                     @"ms0/ la the nho (file save). report.txt ghi toc do va ly do dung, gui kem khi bao loi.\n"
                     @"Hinh ve bang GPU (Metal) o do phan giai 4x PSP. Muon doi: tao file resolution.txt chi chua so 1, 2, 3 hoac 4.\n"
                     @"Phim anime (mod Cutscenes Restored, co phu de): chep nguyen thu muc mod (co Package.xml, data, FMV) vao day; app tu xep vao mods/ khi mo. Chi tuyen nam. Bam START de bo qua phim.\n"
                     @"Cham man hinh (ngoai cac nut): cham 1 ngon = X, cham 2 ngon = O, vuot = phim huong. Muon tat: tao file rong ten tat-cam-ung.txt.\n"
                     @"Tat Data Install trong Config cua game (du lieu da cai se de len file mod).\n"
                     @"Cach cu: file .SFD cua P3 FES trong thu muc movies/ (chi dung khi khong cai mod).\n"
                     @"Neu hinh bi loi, tao file rong ten cpu-renderer.txt trong thu muc nay roi mo lai app de ve bang CPU (cham hon).\n";
    [text writeToFile:note atomically:YES encoding:NSUTF8StringEncoding error:nil];

    _audio = [[P3PAudio alloc] init];
    [_audio start];

    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
    [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    [UIApplication sharedApplication].idleTimerDisabled = YES;
}

- (NSString *)findISO {
    NSString *documents = [self documentsPath];
    NSArray<NSString *> *names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:documents error:nil];
    for (NSString *name in [names sortedArrayUsingSelector:@selector(compare:)])
        if ([[name.pathExtension lowercaseString] isEqualToString:@"iso"]) return [documents stringByAppendingPathComponent:name];
    return nil;
}

- (void)showMessage:(NSString *)text {
    _message.text = text;
    _message.hidden = text.length == 0;
}

- (void)startGameIfPossible {
    if (_started) return;
    NSString *elf = [[NSBundle mainBundle] pathForResource:@"eboot" ofType:@"elf"];
    if (elf == nil) {
        [self showMessage:@"Thiếu eboot.elf trong app (lỗi đóng gói)."];
        return;
    }
    NSString *iso = [self findISO];
    if (iso == nil) {
        [self showMessage:@"Chưa có file ISO.\n\nMở app Tệp (Files) → Trên iPhone → Persona 3 P,\nchép file ISO Persona 3 Portable (ULUS-10512) của bạn vào đó.\nApp sẽ tự chạy khi thấy file."];
        return;
    }
    install_mod_packages([self documentsPath].UTF8String);
    p3pios::Config config;
    config.elf_path = elf.UTF8String;
    config.iso_path = iso.UTF8String;
    config.data_dir = [self documentsPath].UTF8String;
    config.build_id = P3P_BUILD_ID;
    config.movie_dir = config.data_dir + "/movies";
    // The GPU (Metal) draws unless the user put cpu-renderer.txt next to the ISO.
    // Its resolution is 4x the PSP's unless resolution.txt holds 1, 2 or 3.
    std::error_code ec;
    if (!std::filesystem::exists(config.data_dir + "/cpu-renderer.txt", ec)) {
        int scale = 4;
        std::ifstream in(config.data_dir + "/resolution.txt");
        int wanted = 0;
        if (in >> wanted && wanted >= 1 && wanted <= 4) scale = wanted;
        CAMetalLayer *layer = (CAMetalLayer *)_metalView.layer;
        const CGFloat factor = _metalView.contentScaleFactor;
        layer.drawableSize = CGSizeMake(_metalView.bounds.size.width * factor, _metalView.bounds.size.height * factor);
        config.gpu_presents = true;
        config.make_gpu_backend = [layer, scale](std::string &error) { return p3pios::gpu::make_metal_backend(error, (__bridge void *)layer, scale); };
    }
    _runner = std::make_unique<p3pios::LiveRunner>();
    std::string error;
    if (!_runner->start(config, error)) {
        [self showMessage:[NSString stringWithFormat:@"Không khởi động được: %s", error.c_str()]];
        _runner.reset();
        return;
    }
    _started = YES;
    _usesGpuDisplay = config.gpu_presents;
    _metalView.hidden = !_usesGpuDisplay;
    _screen.hidden = _usesGpuDisplay;
    _controls.runner = _runner.get();
    _controls.gesturesEnabled = !std::filesystem::exists(config.data_dir + "/tat-cam-ung.txt", ec);
    gAudioRunner.store(_runner.get(), std::memory_order_release);
    [self showMessage:@"Đang nạp game…"];
}

- (void)presentFrame {
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, _frame.data(), static_cast<CFIndex>(_frame.size()));
    if (data == NULL) return;
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGImageRef image = CGImageCreate(p3pios::kScreenWidth, p3pios::kScreenHeight, 8, 32, p3pios::kScreenWidth * 4u, _colorSpace,
                                     static_cast<CGBitmapInfo>(kCGBitmapByteOrderDefault) | static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipLast),
                                     provider, NULL, false, kCGRenderingIntentDefault);
    if (image != NULL) {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        _screen.layer.contents = (__bridge id)image;
        [CATransaction commit];
        CGImageRelease(image);
    }
    CGDataProviderRelease(provider);
    CFRelease(data);
}

- (void)showSaveDialog:(const p3pios::SaveDialog &)dialog {
    const BOOL load = dialog.kind == p3pios::SaveDialog::Kind::Load;
    const BOOL save = dialog.kind == p3pios::SaveDialog::Kind::Save;
    NSString *title = load ? @"Tải game" : save ? @"Lưu game" : @"Xóa file save";
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:nil preferredStyle:UIAlertControllerStyleAlert];
    _controls.inputEnabled = NO;
    [_controls releaseAll];
    __weak P3PViewController *weakSelf = self;
    for (std::size_t i = 0; i < dialog.slots.size(); ++i) {
        const auto &slot = dialog.slots[i];
        if (load && !slot.exists) continue;
        NSString *detail = slot.exists ? ([NSString stringWithUTF8String:slot.text.c_str()] ?: @"(save)") : @"— trống —";
        NSString *label = [NSString stringWithFormat:@"%02d  %@", static_cast<int>(i) + 1, detail];
        const int index = static_cast<int>(i);
        const BOOL confirm = !load && slot.exists;
        [alert addAction:[UIAlertAction actionWithTitle:label style:UIAlertActionStyleDefault handler:^(UIAlertAction *) {
            P3PViewController *strong = weakSelf;
            if (strong == nil) return;
            if (confirm) [strong confirmSlot:index title:save ? @"Ghi đè file save này?" : @"Xóa file save này?"];
            else [strong finishDialog:index];
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Hủy" style:UIAlertActionStyleCancel handler:^(UIAlertAction *) {
        [weakSelf finishDialog:-1];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)confirmSlot:(int)index title:(NSString *)title {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:nil preferredStyle:UIAlertControllerStyleAlert];
    __weak P3PViewController *weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Đồng ý" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *) {
        [weakSelf finishDialog:index];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Hủy" style:UIAlertActionStyleCancel handler:^(UIAlertAction *) {
        [weakSelf finishDialog:-1];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)finishDialog:(int)index {
    if (_runner) _runner->resolve_save_dialog(index);
    _controls.inputEnabled = YES;
}

- (void)tick:(CADisplayLink *)link {
    ++_ticks;
    if (!_started) {
        if (_ticks % 60u == 1u) [self startGameIfPossible];
        return;
    }
    const std::uint64_t serial = _runner->copy_frame(_usesGpuDisplay ? nullptr : _frame.data(), _serial);
    if (serial != _serial) {
        if (_serial == 0u) [self showMessage:@""];
        _serial = serial;
        if (!_usesGpuDisplay) [self presentFrame];
    }
    p3pios::SaveDialog dialog;
    if (self.presentedViewController == nil && _runner->pending_save_dialog(dialog) && dialog.serial != _dialogSerial) {
        _dialogSerial = dialog.serial;
        [self showSaveDialog:dialog];
    }
    if (_ticks % 30u == 0u) {
        const p3pios::Status status = _runner->status();
        _statusLabel.text = [NSString stringWithFormat:@"%.0f fps  %.0f%%  %s  %s", status.fps, status.speed * 100.0,
                                                       status.renderer.rfind("gpu", 0) == 0 ? "GPU" : "CPU", P3P_BUILD_ID];
        if (status.finished && !_reportedEnd) {
            _reportedEnd = YES;
            const std::string report = _runner->report();
            [self showMessage:[NSString stringWithFormat:@"Game đã dừng.\n\n%@\n(Chi tiết: report.txt trong thư mục của app)",
                                                         [NSString stringWithUTF8String:report.c_str()] ?: @""]];
            _message.font = [UIFont monospacedSystemFontOfSize:10.0 weight:UIFontWeightRegular];
            _message.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.75];
        }
    }
}

- (void)applicationActive:(BOOL)active {
    if (_runner) _runner->set_paused(!active);
    if (active) [_audio resume];
    else [_controls releaseAll];
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }

@end

// ---------------------------------------------------------------------------
// Application and scene.
// ---------------------------------------------------------------------------
@interface P3PSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation P3PSceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    if (![scene isKindOfClass:[UIWindowScene class]]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [[P3PViewController alloc] init];
    [self.window makeKeyAndVisible];
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
    [(P3PViewController *)self.window.rootViewController applicationActive:YES];
}

- (void)sceneWillResignActive:(UIScene *)scene {
    [(P3PViewController *)self.window.rootViewController applicationActive:NO];
}

@end

@interface P3PAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation P3PAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    return YES;
}

- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)connectingSceneSession
                                   options:(UISceneConnectionOptions *)options {
    UISceneConfiguration *configuration = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:connectingSceneSession.role];
    configuration.delegateClass = [P3PSceneDelegate class];
    return configuration;
}

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([P3PAppDelegate class]));
    }
}
