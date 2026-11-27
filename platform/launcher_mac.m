// The machine launcher window (AppKit, built in code; no nib). See launcher.h.
#import <Cocoa/Cocoa.h>
#include "launcher.h"

static NSString *S(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

static void copy_str(char *dst, size_t len, NSString *s)
{
    snprintf(dst, len, "%s", s.UTF8String ? s.UTF8String : "");
}

static void snapshot(NSWindow *w, const char *path)
{
    NSView *v = w.contentView.superview ? w.contentView.superview : w.contentView;
    [v layoutSubtreeIfNeeded];
    [v displayIfNeeded];
    NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
    [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    [png writeToFile:S(path) atomically:YES];
}

static NSTextField *label(NSString *text)
{
    NSTextField *t = [NSTextField labelWithString:text];
    t.alignment = NSTextAlignmentRight;
    return t;
}

static NSPopUpButton *popup(NSArray<NSString *> *items, NSInteger sel)
{
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [p addItemsWithTitles:items];
    [p selectItemAtIndex:sel];
    return p;
}

static const int RAM_SIZES[] = { 64, 128, 256, 512, 1024 };
enum { N_RAM = sizeof RAM_SIZES / sizeof RAM_SIZES[0] };

/* ---- edit sheet ---- */

@interface BMEditor : NSObject
@property (strong) NSWindow *window;
@end

@implementation BMEditor {
    vm_config_t _vm;
    NSTextField *_name, *_hda, *_cd, *_note;
    NSPopUpButton *_boot, *_ram, *_cpus, *_video, *_sound, *_nic;
    NSButton *_writable, *_clearHda, *_clearCd;
    NSMutableArray<NSNumber *> *_ramValues;
}

- (instancetype)initWithVM:(const vm_config_t *)vm title:(NSString *)title
{
    if (!(self = [super init])) return nil;
    _vm = *vm;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 560, 420)
                                          styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    _window.title = title;

    _name = [NSTextField textFieldWithString:S(vm->name)];
    _name.placeholderString = @"Name";
    [_name.widthAnchor constraintGreaterThanOrEqualToConstant:300].active = YES;

    _hda = [NSTextField labelWithString:@""];
    _cd = [NSTextField labelWithString:@""];
    for (NSTextField *f in @[ _hda, _cd ]) {
        f.lineBreakMode = NSLineBreakByTruncatingMiddle;
        [f setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
        [f.widthAnchor constraintEqualToConstant:300].active = YES;
    }
    NSButton *chooseHda = [NSButton buttonWithTitle:@"Choose…" target:self action:@selector(chooseHda:)];
    NSButton *chooseCd = [NSButton buttonWithTitle:@"Choose…" target:self action:@selector(chooseCd:)];
    _clearHda = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearHda:)];
    _clearCd = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearCd:)];
    NSStackView *hdaRow = [NSStackView stackViewWithViews:@[ _hda, chooseHda, _clearHda ]];
    NSStackView *cdRow = [NSStackView stackViewWithViews:@[ _cd, chooseCd, _clearCd ]];

    _boot = popup(@[ @"Hard disk", @"CD-ROM" ], vm->boot_cd ? 1 : 0);

    _ramValues = [NSMutableArray array];
    for (int i = 0; i < N_RAM; i++) [_ramValues addObject:@(RAM_SIZES[i])];
    if (![_ramValues containsObject:@(vm->ram_mb)]) {
        [_ramValues addObject:@(vm->ram_mb)];
        [_ramValues sortUsingSelector:@selector(compare:)];
    }
    NSMutableArray *ramTitles = [NSMutableArray array];
    for (NSNumber *n in _ramValues) [ramTitles addObject:[NSString stringWithFormat:@"%@ MB", n]];
    _ram = popup(ramTitles, (NSInteger)[_ramValues indexOfObject:@(vm->ram_mb)]);

    NSMutableArray *cpuTitles = [NSMutableArray array];
    for (int i = 1; i <= 8; i++) [cpuTitles addObject:[NSString stringWithFormat:@"%d", i]];
    _cpus = popup(cpuTitles, vm->cpus - 1);
    _cpus.target = self;
    _cpus.action = @selector(update:);

    _video = popup(@[ @"VESA (standard)", @"Matrox G400 (R5's g400 driver)" ], vm->video);
    _sound = popup(@[ @"Ensoniq ES1370 (AudioPCI)", @"Sound Blaster 16 PnP (R5 only)", @"None" ], vm->sound);
    _nic = popup(@[ @"RTL8139 + NE2000 PCI (both)", @"RealTek RTL8139", @"NE2000 PCI (RTL8029)", @"None" ], vm->nic);

    _writable = [NSButton checkboxWithTitle:@"Save changes to the disk image" target:self action:@selector(update:)];
    _writable.state = vm->writable ? NSControlStateValueOn : NSControlStateValueOff;

    _note = [NSTextField wrappingLabelWithString:@""];
    _note.textColor = NSColor.secondaryLabelColor;
    _note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    [_note.widthAnchor constraintEqualToConstant:380].active = YES;

    NSGridView *grid = [NSGridView gridViewWithViews:@[
        @[ label(@"Name:"), _name ],
        @[ label(@"Hard disk:"), hdaRow ],
        @[ label(@"CD-ROM:"), cdRow ],
        @[ label(@"Start from:"), _boot ],
        @[ label(@"Memory:"), _ram ],
        @[ label(@"Processors:"), _cpus ],
        @[ label(@"Video card:"), _video ],
        @[ label(@"Sound card:"), _sound ],
        @[ label(@"Network card:"), _nic ],
        @[ NSGridCell.emptyContentView, _writable ],
        @[ NSGridCell.emptyContentView, _note ],
    ]];
    grid.rowSpacing = 8;
    grid.columnSpacing = 8;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    for (NSInteger r = 0; r < grid.numberOfRows; r++) [grid rowAtIndex:r].yPlacement = NSGridCellPlacementCenter;
    [grid rowAtIndex:grid.numberOfRows - 1].yPlacement = NSGridCellPlacementTop;

    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];
    cancel.keyEquivalent = @"\e";
    NSButton *save = [NSButton buttonWithTitle:@"Save" target:self action:@selector(save:)];
    save.keyEquivalent = @"\r";
    NSStackView *buttons = [NSStackView stackViewWithViews:@[ cancel, save ]];

    NSView *content = _window.contentView;
    for (NSView *v in @[ grid, buttons ]) {
        v.translatesAutoresizingMaskIntoConstraints = NO;
        [content addSubview:v];
    }
    [NSLayoutConstraint activateConstraints:@[
        [grid.topAnchor constraintEqualToAnchor:content.topAnchor constant:20],
        [grid.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],
        [grid.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],
        [buttons.topAnchor constraintEqualToAnchor:grid.bottomAnchor constant:16],
        [buttons.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],
        [buttons.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-20],
    ]];
    [self showPaths];
    [self update:nil];
    _window.initialFirstResponder = _name;
    return self;
}

- (void)showPaths
{
    _hda.stringValue = _vm.hda[0] ? S(_vm.hda) : @"None";
    _cd.stringValue = _vm.cdrom[0] ? S(_vm.cdrom) : @"None";
    _hda.toolTip = _vm.hda[0] ? S(_vm.hda) : nil;
    _cd.toolTip = _vm.cdrom[0] ? S(_vm.cdrom) : nil;
    _hda.textColor = _vm.hda[0] ? NSColor.labelColor : NSColor.secondaryLabelColor;
    _cd.textColor = _vm.cdrom[0] ? NSColor.labelColor : NSColor.secondaryLabelColor;
    _clearHda.enabled = _vm.hda[0] != 0;
    _clearCd.enabled = _vm.cdrom[0] != 0;
}

- (void)update:(id)sender
{
    (void)sender;
    NSMutableArray *lines = [NSMutableArray array];
    if (_cpus.indexOfSelectedItem > 0)
        [lines addObject:@"With more than one processor, idle processors keep the Mac busy unless BeOS is told it "
                         @"may halt them: add the line \"hlt enabled\" to "
                         @"/boot/home/config/settings/kernel/drivers/kernel (a stock BeOS setting)."];
    if (_writable.state == NSControlStateValueOn)
        [lines addObject:@"BeOS writes to the image itself. Keep a copy of it."];
    else
        [lines addObject:@"Changes BeOS makes to the disk are discarded when the machine stops."];
    _note.stringValue = [lines componentsJoinedByString:@"\n\n"];
}

- (NSString *)pickWithTitle:(NSString *)title current:(const char *)cur
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.message = title;
    p.canChooseDirectories = NO;
    p.allowsMultipleSelection = NO;
    if (cur[0]) p.directoryURL = [NSURL fileURLWithPath:S(cur)].URLByDeletingLastPathComponent;
    return [p runModal] == NSModalResponseOK ? p.URL.path : nil;
}

- (void)chooseHda:(id)sender
{
    (void)sender;
    NSString *path = [self pickWithTitle:@"Choose a raw BeOS hard disk image" current:_vm.hda];
    if (path) { copy_str(_vm.hda, sizeof _vm.hda, path); [self showPaths]; }
}

- (void)chooseCd:(id)sender
{
    (void)sender;
    NSString *path = [self pickWithTitle:@"Choose a CD image (.iso)" current:_vm.cdrom];
    if (path) {
        copy_str(_vm.cdrom, sizeof _vm.cdrom, path);
        if (!_vm.hda[0]) [_boot selectItemAtIndex:1];
        [self showPaths];
    }
}

- (void)clearHda:(id)sender { (void)sender; _vm.hda[0] = 0; [self showPaths]; }
- (void)clearCd:(id)sender { (void)sender; _vm.cdrom[0] = 0; [_boot selectItemAtIndex:0]; [self showPaths]; }

- (void)cancel:(id)sender
{
    (void)sender;
    [_window.sheetParent endSheet:_window returnCode:NSModalResponseCancel];
}

- (void)save:(id)sender
{
    (void)sender;
    NSString *name = [_name.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    NSString *problem = nil;
    if (!name.length) problem = @"Give the machine a name.";
    else if (!_vm.hda[0] && !_vm.cdrom[0]) problem = @"Choose a hard disk image or a CD image.";
    else if (_boot.indexOfSelectedItem == 1 && !_vm.cdrom[0]) problem = @"Choose a CD image to start from, or start from the hard disk.";
    else if (_boot.indexOfSelectedItem == 0 && !_vm.hda[0]) problem = @"Choose a hard disk image to start from, or start from the CD-ROM.";
    if (problem) {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = problem;
        [a beginSheetModalForWindow:_window completionHandler:nil];
        return;
    }
    copy_str(_vm.name, sizeof _vm.name, name);
    _vm.boot_cd = _boot.indexOfSelectedItem == 1;
    _vm.ram_mb = _ramValues[(NSUInteger)_ram.indexOfSelectedItem].intValue;
    _vm.cpus = (int)_cpus.indexOfSelectedItem + 1;
    _vm.video = (int)_video.indexOfSelectedItem;
    _vm.sound = (int)_sound.indexOfSelectedItem;
    _vm.nic = (int)_nic.indexOfSelectedItem;
    _vm.writable = _writable.state == NSControlStateValueOn;
    [_window.sheetParent endSheet:_window returnCode:NSModalResponseOK];
}

- (const vm_config_t *)vm { return &_vm; }
@end

/* ---- machine list ---- */

@interface BMLauncher : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate>
@property (strong) NSWindow *window;
@end

@implementation BMLauncher {
    const char *_dir;
    vm_config_t _vms[VM_MAX];
    int _n;
    NSTableView *_table;
    NSTextField *_empty;
    NSButton *_edit, *_dup, *_del, *_start;
    BMEditor *_editor;
    @public int _chosen;
}

- (instancetype)initWithDir:(const char *)dir
{
    if (!(self = [super init])) return nil;
    _dir = dir;
    _chosen = -1;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 620, 400)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"beemu";
    _window.minSize = NSMakeSize(520, 300);
    _window.delegate = self;

    NSTextField *heading = [NSTextField labelWithString:@"Choose a machine to start"];
    heading.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize + 2];

    _table = [[NSTableView alloc] initWithFrame:NSZeroRect];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"vm"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _table.headerView = nil;
    _table.rowHeight = 46;
    _table.style = NSTableViewStyleInset;
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.doubleAction = @selector(start:);
    _table.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.documentView = _table;
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;

    _empty = [NSTextField wrappingLabelWithString:@"No machines yet. Click New… and choose a BeOS disk image "
                                                  @"(raw; convert QCOW2 with qemu-img) or an install CD."];
    _empty.alignment = NSTextAlignmentCenter;
    _empty.textColor = NSColor.secondaryLabelColor;

    NSButton *add = [NSButton buttonWithTitle:@"New…" target:self action:@selector(add:)];
    _edit = [NSButton buttonWithTitle:@"Edit…" target:self action:@selector(edit:)];
    _dup = [NSButton buttonWithTitle:@"Duplicate" target:self action:@selector(duplicate:)];
    _del = [NSButton buttonWithTitle:@"Delete" target:self action:@selector(remove:)];
    NSButton *quit = [NSButton buttonWithTitle:@"Quit" target:self action:@selector(quit:)];
    _start = [NSButton buttonWithTitle:@"Start" target:self action:@selector(start:)];
    _start.keyEquivalent = @"\r";
    NSStackView *left = [NSStackView stackViewWithViews:@[ add, _edit, _dup, _del ]];
    NSStackView *right = [NSStackView stackViewWithViews:@[ quit, _start ]];

    NSView *content = _window.contentView;
    for (NSView *v in @[ heading, scroll, _empty, left, right ]) {
        v.translatesAutoresizingMaskIntoConstraints = NO;
        [content addSubview:v];
    }
    [NSLayoutConstraint activateConstraints:@[
        [heading.topAnchor constraintEqualToAnchor:content.topAnchor constant:16],
        [heading.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],
        [scroll.topAnchor constraintEqualToAnchor:heading.bottomAnchor constant:10],
        [scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],
        [scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],
        [_empty.centerXAnchor constraintEqualToAnchor:scroll.centerXAnchor],
        [_empty.centerYAnchor constraintEqualToAnchor:scroll.centerYAnchor],
        [_empty.widthAnchor constraintEqualToConstant:360],
        [left.topAnchor constraintEqualToAnchor:scroll.bottomAnchor constant:14],
        [left.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],
        [left.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-18],
        [right.centerYAnchor constraintEqualToAnchor:left.centerYAnchor],
        [right.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],
    ]];
    return self;
}

- (void)reloadSelecting:(const char *)name
{
    _n = vm_list(_dir, _vms, VM_MAX);
    [_table reloadData];
    int sel = _n ? 0 : -1;
    for (int i = 0; name && i < _n; i++)
        if (!strcmp(_vms[i].name, name)) sel = i;
    if (sel >= 0) {
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)sel] byExtendingSelection:NO];
        [_table scrollRowToVisible:sel];
    }
    _empty.hidden = _n > 0;
    [self updateButtons];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv { (void)tv; return _n; }

static NSString *detail(const vm_config_t *vm)
{
    NSMutableArray *parts = [NSMutableArray array];
    if (vm->hda[0]) [parts addObject:S(vm->hda).lastPathComponent];
    if (vm->cdrom[0]) [parts addObject:[NSString stringWithFormat:@"CD %@", S(vm->cdrom).lastPathComponent]];
    [parts addObject:[NSString stringWithFormat:@"%d MB", vm->ram_mb]];
    [parts addObject:vm->cpus == 1 ? @"1 CPU" : [NSString stringWithFormat:@"%d CPUs", vm->cpus]];
    [parts addObject:vm->video == VM_VIDEO_G400 ? @"G400" : @"VESA"];
    [parts addObject:vm->sound == VM_SOUND_SB16 ? @"SB16" : vm->sound == VM_SOUND_NONE ? @"no sound" : @"ES1370"];
    if (vm->writable) [parts addObject:@"saves to disk"];
    return [parts componentsJoinedByString:@" · "];
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row
{
    (void)col;
    NSTableCellView *cell = [tv makeViewWithIdentifier:@"vmcell" owner:self];
    if (!cell) {
        cell = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
        cell.identifier = @"vmcell";
        NSTextField *name = [NSTextField labelWithString:@""];
        name.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
        name.tag = 1;
        NSTextField *info = [NSTextField labelWithString:@""];
        info.textColor = NSColor.secondaryLabelColor;
        info.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        info.lineBreakMode = NSLineBreakByTruncatingMiddle;
        info.tag = 2;
        for (NSTextField *f in @[ name, info ]) {
            f.translatesAutoresizingMaskIntoConstraints = NO;
            [f setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
            [cell addSubview:f];
            [f.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:6].active = YES;
            [f.trailingAnchor constraintLessThanOrEqualToAnchor:cell.trailingAnchor constant:-6].active = YES;
        }
        [name.topAnchor constraintEqualToAnchor:cell.topAnchor constant:5].active = YES;
        [info.topAnchor constraintEqualToAnchor:name.bottomAnchor constant:2].active = YES;
        cell.textField = name;
    }
    const vm_config_t *vm = &_vms[row];
    ((NSTextField *)[cell viewWithTag:1]).stringValue = S(vm->name);
    ((NSTextField *)[cell viewWithTag:2]).stringValue = detail(vm);
    return cell;
}

- (void)tableViewSelectionDidChange:(NSNotification *)n { (void)n; [self updateButtons]; }

- (void)updateButtons
{
    BOOL any = _table.selectedRow >= 0;
    _edit.enabled = _dup.enabled = _del.enabled = _start.enabled = any;
}

- (const vm_config_t *)selected
{
    NSInteger r = _table.selectedRow;
    return r >= 0 && r < _n ? &_vms[r] : NULL;
}

- (void)alert:(NSString *)text
{
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = text;
    [a beginSheetModalForWindow:_window completionHandler:nil];
}

// Edit sheet for vm; old_name "" for a new machine.
- (void)runEditor:(const vm_config_t *)vm oldName:(NSString *)oldName title:(NSString *)title
{
    _editor = [[BMEditor alloc] initWithVM:vm title:title];
    BMEditor *ed = _editor;
    [_window beginSheet:ed.window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        const vm_config_t *nv = [ed vm];
        char err[512] = "";
        bool ok;
        vm_config_t other;
        if (!oldName.length || strcmp(oldName.UTF8String, nv->name)) {
            if (vm_load(self->_dir, nv->name, &other)) {
                [self alert:[NSString stringWithFormat:@"There is already a machine called “%@”.", S(nv->name)]];
                return;
            }
        }
        ok = oldName.length ? vm_rename(self->_dir, oldName.UTF8String, nv, err, sizeof err)
                            : vm_save(self->_dir, nv, err, sizeof err);
        if (!ok) { [self alert:S(err)]; return; }
        [self reloadSelecting:nv->name];
    }];
}

- (void)add:(id)sender
{
    (void)sender;
    vm_config_t vm;
    vm_defaults(&vm);
    // A free name: BeOS, BeOS 2, ...
    vm_config_t other;
    for (int i = 2; vm_load(_dir, vm.name, &other) && i < 100; i++) snprintf(vm.name, sizeof vm.name, "BeOS %d", i);
    [self runEditor:&vm oldName:@"" title:@"New machine"];
}

- (void)edit:(id)sender
{
    (void)sender;
    const vm_config_t *vm = [self selected];
    if (vm) [self runEditor:vm oldName:S(vm->name) title:@"Edit machine"];
}

- (void)duplicate:(id)sender
{
    (void)sender;
    const vm_config_t *vm = [self selected];
    if (!vm) return;
    vm_config_t copy = *vm, other;
    for (int i = 1; i < 100; i++) {
        if (i == 1) snprintf(copy.name, sizeof copy.name, "%s copy", vm->name);
        else snprintf(copy.name, sizeof copy.name, "%s copy %d", vm->name, i);
        if (!vm_load(_dir, copy.name, &other)) break;
    }
    char err[512];
    if (!vm_save(_dir, &copy, err, sizeof err)) { [self alert:S(err)]; return; }
    [self reloadSelecting:copy.name];
}

- (void)remove:(id)sender
{
    (void)sender;
    const vm_config_t *vm = [self selected];
    if (!vm) return;
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"Delete the machine “%@”?", S(vm->name)];
    a.informativeText = @"Only its settings are removed. The disk and CD images are not touched.";
    [a addButtonWithTitle:@"Delete"];
    [a addButtonWithTitle:@"Cancel"];
    a.buttons[0].hasDestructiveAction = YES;
    char name[sizeof vm->name];
    snprintf(name, sizeof name, "%s", vm->name);
    NSString *keep = S(name);
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSAlertFirstButtonReturn) return;
        vm_delete(self->_dir, keep.UTF8String);
        [self reloadSelecting:NULL];
    }];
}

- (void)start:(id)sender
{
    (void)sender;
    if (_window.attachedSheet) return;
    NSInteger r = _table.selectedRow;
    if (r < 0 || r >= _n) return;
    _chosen = (int)r;
    [NSApp stopModalWithCode:NSModalResponseOK];
}

- (void)quit:(id)sender
{
    (void)sender;
    [NSApp stopModalWithCode:NSModalResponseCancel];
}

- (BOOL)windowShouldClose:(NSWindow *)w
{
    (void)w;
    [NSApp stopModalWithCode:NSModalResponseCancel];
    return YES;
}

- (const vm_config_t *)chosen { return _chosen >= 0 ? &_vms[_chosen] : NULL; }

- (void)snapshotEditTo:(const char *)path
{
    const vm_config_t *vm = [self selected];
    vm_config_t d;
    if (!vm) { vm_defaults(&d); vm = &d; }
    BMEditor *ed = [[BMEditor alloc] initWithVM:vm title:@"Edit machine"];
    snapshot(ed.window, path);
}
@end

bool platform_launcher(const char *dir, const char *select, vm_config_t *out)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        BMLauncher *l = [[BMLauncher alloc] initWithDir:dir];
        [l reloadSelecting:select];
        const char *snap = getenv("BEEMU_LAUNCHER_SNAPSHOT");
        if (snap) {
            snapshot(l.window, snap);
            const char *snap_edit = getenv("BEEMU_LAUNCHER_SNAPSHOT_EDIT");
            if (snap_edit) [l snapshotEditTo:snap_edit];
            return false;
        }
        if (getenv("BEEMU_LAUNCHER_AUTOSTART"))     // tests: start the selection after half a second
            [l performSelector:@selector(start:) withObject:nil afterDelay:0.5 inModes:@[ NSModalPanelRunLoopMode ]];
        [l.window center];
        [l.window makeKeyAndOrderFront:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
        NSModalResponse r = [NSApp runModalForWindow:l.window];
        [l.window orderOut:nil];
        const vm_config_t *vm = [l chosen];
        if (r != NSModalResponseOK || !vm) return false;
        *out = *vm;
        return true;
    }
}
