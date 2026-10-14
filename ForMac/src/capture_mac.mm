#include "platform.h"
#include <QScreen>
#include <QMutex>
#include <QMutexLocker>
#include <QElapsedTimer>
#import <AppKit/AppKit.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <cstring>
@interface LinkDeskCapture : NSObject<SCStreamOutput,SCStreamDelegate>
@property(nonatomic,strong) SCStream *stream;
@property(atomic) BOOL active;
@end
// Recursive to tolerate immediate framework completion callbacks on startup.
namespace { QRecursiveMutex latestMutex; QImage latest; quint64 serial=0; QRegion latestDirty;bool latestRegions=false,failed=false;LinkDeskCapture *currentCapture=nil;CGDirectDisplayID selectedDisplay=0; }
@implementation LinkDeskCapture
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    if(!self.active) return;
    if(type!=SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample)) return;
    CVPixelBufferRef pixels=CMSampleBufferGetImageBuffer(sample); if(!pixels) return;
    const auto attachments=CMSampleBufferGetSampleAttachmentsArray(sample,false);
    QRegion dirty;bool hasRegions=false;
    if(attachments && CFArrayGetCount(attachments)) { NSDictionary *info=(__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments,0); if([info[SCStreamFrameInfoStatus] integerValue]!=SCFrameStatusComplete) return; }
    if(attachments && CFArrayGetCount(attachments)) { NSDictionary *info=(__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments,0);NSArray *rectangles=info[SCStreamFrameInfoDirtyRects];
        // Retina/scaled dirty coordinates fall back to exact full comparison.
        if([rectangles isKindOfClass:[NSArray class]] && [info[SCStreamFrameInfoScaleFactor] doubleValue]==1.0 && [info[SCStreamFrameInfoContentScale] doubleValue]==1.0) { hasRegions=true;for(id value in rectangles) {
            CGRect r=CGRectZero;
            if([value isKindOfClass:[NSValue class]]) r=[value rectValue];
            else if([value isKindOfClass:[NSDictionary class]]) { if(!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)value,&r)) { hasRegions=false;break; } }
            else { hasRegions=false;break; }
            dirty+=QRectF(r.origin.x,r.origin.y,r.size.width,r.size.height).toAlignedRect();
        } }
    }
    CVPixelBufferLockBaseAddress(pixels,kCVPixelBufferLock_ReadOnly);
    QImage image(static_cast<const uchar *>(CVPixelBufferGetBaseAddress(pixels)),int(CVPixelBufferGetWidth(pixels)),int(CVPixelBufferGetHeight(pixels)),qsizetype(CVPixelBufferGetBytesPerRow(pixels)),QImage::Format_RGB32);
    { QMutexLocker guard(&latestMutex); if(self.active) { latest=image.copy();latestDirty=dirty;latestRegions=hasRegions; ++serial; } }
    CVPixelBufferUnlockBaseAddress(pixels,kCVPixelBufferLock_ReadOnly);
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error { QMutexLocker guard(&latestMutex);if(self.active) failed=true; }
@end
CaptureFrame Platform::captureFrame(QScreen *screen,const QSize &maximum) {
    static quint64 lastSerial=0;
    static QElapsedTimer startup;
    if(currentCapture && selectedDisplay!=CGMainDisplayID()) stopCapture();
    if(!currentCapture) {
        startup.start();selectedDisplay=CGMainDisplayID();currentCapture=[LinkDeskCapture new];currentCapture.active=YES;LinkDeskCapture *capture=currentCapture;const auto displayId=selectedDisplay;
        [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:YES completionHandler:^(SCShareableContent *content,NSError *error) {
            QMutexLocker startupGuard(&latestMutex);if(!capture.active) return;
            SCDisplay *selected=nil;
            for(SCDisplay *display in content.displays) if(display.displayID==displayId) { selected=display; break; }
            if(!selected || error) { QMutexLocker guard(&latestMutex);failed=true;return; }
            SCContentFilter *filter=[[SCContentFilter alloc] initWithDisplay:selected excludingWindows:@[]];
            SCStreamConfiguration *config=[SCStreamConfiguration new]; config.width=selected.width;config.height=selected.height;config.pixelFormat=kCVPixelFormatType_32BGRA;config.showsCursor=NO;config.minimumFrameInterval=CMTimeMake(1,30);config.queueDepth=2;
            capture.stream=[[SCStream alloc] initWithFilter:filter configuration:config delegate:capture];
            NSError *failure=nil; [capture.stream addStreamOutput:capture type:SCStreamOutputTypeScreen sampleHandlerQueue:dispatch_queue_create("LinkDesk.ScreenCapture",DISPATCH_QUEUE_SERIAL) error:&failure];
            if(!failure) [capture.stream startCaptureWithCompletionHandler:^(NSError *error){QMutexLocker guard(&latestMutex);if(error && capture.active) failed=true;}];
            else { QMutexLocker guard(&latestMutex);failed=true; }
        }];
    }
    CaptureFrame frame;
    { QMutexLocker guard(&latestMutex); if(failed) return frame;frame.image=latest;frame.serial=serial;frame.previousSerial=serial ? serial-1 : 0;frame.dirty=latestDirty;frame.regionsValid=latestRegions && !latest.isNull() && latest.rect().contains(latestDirty.boundingRect());frame.unchanged=serial && serial==lastSerial;lastSerial=serial; }
    if(frame.image.isNull()) {
        Q_UNUSED(screen);frame.pending=startup.elapsed()<10000;
    }
    else if(frame.image.width()>maximum.width() || frame.image.height()>maximum.height()) { frame.image=frame.image.scaled(maximum,Qt::KeepAspectRatio,Qt::SmoothTransformation);frame.regionsValid=false;frame.moves.clear(); }
    return frame;
}
CursorFrame Platform::captureCursor(const QRect &geometry) {
    CursorFrame frame; if(geometry.isEmpty()) return frame;
    CGEventRef event=CGEventCreate(nullptr); if(!event) return frame; const auto position=CGEventGetLocation(event);CFRelease(event);
    frame.position=QPointF((position.x-geometry.x())/geometry.width(),(position.y-geometry.y())/geometry.height());frame.visible=geometry.contains(QPoint(int(position.x),int(position.y)));
    NSCursor *cursor=[NSCursor currentSystemCursor]; if(!cursor) { frame.visible=false;return frame; }
    static NSCursor *previous=nil;static QImage shape;
    if(cursor!=previous) {
        NSRect rect=NSMakeRect(0,0,cursor.image.size.width,cursor.image.size.height);CGImageRef raster=[cursor.image CGImageForProposedRect:&rect context:nil hints:nil];
        if(raster) { NSBitmapImageRep *bitmap=[[NSBitmapImageRep alloc] initWithCGImage:raster];NSData *data=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];shape=QImage::fromData(static_cast<const uchar *>(data.bytes),int(data.length)).scaled(qBound(1,int(cursor.image.size.width),128),qBound(1,int(cursor.image.size.height),128),Qt::KeepAspectRatio,Qt::SmoothTransformation); } else shape={};previous=cursor;
    }
    frame.image=shape;
    const NSPoint hotspot=cursor.hotSpot; const NSSize original=cursor.image.size;
    if(original.width && original.height) frame.hotspot=QPoint(int(hotspot.x*frame.image.width()/original.width),int(hotspot.y*frame.image.height()/original.height));
    return frame;
}
void Platform::stopCapture() {
    LinkDeskCapture *capture;
    { QMutexLocker guard(&latestMutex);capture=currentCapture;capture.active=NO;currentCapture=nil;latest={};latestDirty={};latestRegions=false;failed=false;serial=0; }
    if(capture) { [capture.stream removeStreamOutput:capture type:SCStreamOutputTypeScreen error:NULL];[capture.stream stopCaptureWithCompletionHandler:^(NSError *){}]; }
}
