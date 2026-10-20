// SPDX-License-Identifier: MIT
// Explicit original RenderBox vector commands; rendering must execute on GPU.
#define OFFLINE_RENDERBOX_DRAW 1
#include "OfflineRenderBoxDevice.mm"
#import <CoreGraphics/CoreGraphics.h>
#ifndef OFFLINE_RENDERBOX_WIDTH
#define OFFLINE_RENDERBOX_WIDTH 64
#define OFFLINE_RENDERBOX_HEIGHT 64
#endif
static constexpr unsigned rbWidth=OFFLINE_RENDERBOX_WIDTH,rbHeight=OFFLINE_RENDERBOX_HEIGHT;
#if OFFLINE_RENDERBOX_PRESENTATION
static id<MTLTexture> actualOriginalRenderTarget;
#endif
struct RBFloatColor {float red,green,blue,alpha;};
@interface NSObject (OriginalRenderBoxDrawing)
- (void)setRect:(CGRect)rect;
- (void)setColor:(RBFloatColor)color;
- (void)drawShape:(id)shape fill:(id)fill alpha:(float)alpha blendMode:(int)mode;
- (CGImageRef)renderImageInRect:(CGRect)rect options:(NSDictionary *)options renderer:(void (^)(id))renderer;
@end
static void observeUncaughtDrawingException(NSException *exception) {
 observeFailure([NSString stringWithFormat:@"Actual uncaught %@: %@",exception.name,exception.reason]);
}
// Explicit own-device observation only. Original framework methods are not
// replaced. Call the exact existing own frontend implementation and stop on a
// real nil resource before the original renderer can silently choose a CPU path.
typedef id (*TextureFactory)(id,SEL,MTLTextureDescriptor *) __attribute__((ns_returns_retained));
typedef id (*SurfaceFactory)(id,SEL,MTLTextureDescriptor *,IOSurfaceRef,NSUInteger) __attribute__((ns_returns_retained));
typedef id (*BufferFactory)(id,SEL,NSUInteger,MTLResourceOptions) __attribute__((ns_returns_retained));
typedef id (*PipelineFactory)(id,SEL,MTLRenderPipelineDescriptor *,NSError *__autoreleasing *) __attribute__((ns_returns_retained));
static TextureFactory actualTextureFactory;
static SurfaceFactory actualSurfaceFactory;
static BufferFactory actualBufferFactory;
static PipelineFactory actualPipelineFactory;
__attribute__((noreturn)) static void observeTextureRefusal(MTLTextureDescriptor *d,IOSurfaceRef surface,NSUInteger plane) {
 phase=70;observeFailure([NSString stringWithFormat:@"Actual own texture refusal type %lu format %lu extent %lux%lu depth %lu levels %lu samples %lu array %lu usage %lu storage %lu cache %lu hazard %lu optimized %d surface %u format %u planes %lu plane %lu",
  (unsigned long)d.textureType,(unsigned long)d.pixelFormat,(unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.depth,
  (unsigned long)d.mipmapLevelCount,(unsigned long)d.sampleCount,(unsigned long)d.arrayLength,(unsigned long)d.usage,
  (unsigned long)d.storageMode,(unsigned long)d.cpuCacheMode,(unsigned long)d.hazardTrackingMode,d.allowGPUOptimizedContents,
  surface?IOSurfaceGetID(surface):0,surface?IOSurfaceGetPixelFormat(surface):0,surface?(unsigned long)IOSurfaceGetPlaneCount(surface):0,(unsigned long)plane]);
}
@interface OwnRenderBoxResourceObservation : NSObject
@end
@implementation OwnRenderBoxResourceObservation
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d {
 id value=actualTextureFactory(self,_cmd,d);if(!value)observeTextureRefusal(d,nullptr,0);return value;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane {
 id value=actualSurfaceFactory(self,_cmd,d,surface,plane);if(!value)observeTextureRefusal(d,surface,plane);
#if OFFLINE_RENDERBOX_PRESENTATION
 if(d.pixelFormat==MTLPixelFormatRGBA8Unorm && d.width==rbWidth && d.height==rbHeight && surface)actualOriginalRenderTarget=value;
#endif
 return value;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
 id value=actualBufferFactory(self,_cmd,length,options);
 if(!value){phase=71;observeFailure([NSString stringWithFormat:@"Actual own buffer refusal length %lu options 0x%lx",(unsigned long)length,(unsigned long)options]);}return value;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError *__autoreleasing *)error {
 NSError *failure=nil;id value=actualPipelineFactory(self,_cmd,d,&failure);if(error)*error=failure;
 if(!value){phase=72;observeFailure([NSString stringWithFormat:@"Actual own pipeline refusal vertex %@ fragment %@ color %lu samples %lu depth %lu stencil %lu error %@",
  d.vertexFunction.name,d.fragmentFunction.name,(unsigned long)d.colorAttachments[0].pixelFormat,(unsigned long)d.rasterSampleCount,
  (unsigned long)d.depthAttachmentPixelFormat,(unsigned long)d.stencilAttachmentPixelFormat,failure]);}return value;
}
@end
@interface OwnFunctionConstantObservation : NSObject
@end
@implementation OwnFunctionConstantObservation
- (id)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values error:(NSError **)error {
 (void)error;phase=73;Class cls=object_getClass(values);
 NSMutableString *detail=[NSMutableString stringWithFormat:@"Actual function specialization %@ values class %@ size %lu description %@",name,NSStringFromClass(cls),cls?(unsigned long)class_getInstanceSize(cls):0,values];
 for(Class c=cls;c && c!=[NSObject class];c=class_getSuperclass(c)) {
  unsigned count=0;Method *methods=class_copyMethodList(c,&count);
  [detail appendFormat:@" METHODS %@:",NSStringFromClass(c)];
  for(unsigned i=0;i<count;i++)[detail appendFormat:@" %s{%s}",sel_getName(method_getName(methods[i])),method_getTypeEncoding(methods[i])];free(methods);
  Ivar *ivars=class_copyIvarList(c,&count);[detail appendString:@" IVARS:"];
  for(unsigned i=0;i<count;i++)[detail appendFormat:@" %s{%s}@%ld",ivar_getName(ivars[i]),ivar_getTypeEncoding(ivars[i]),(long)ivar_getOffset(ivars[i])];free(ivars);
 }
 observeFailure(detail);
}
@end
static void observeOwnResources(id<MTLDevice> device) {
 Class base=object_getClass(device);require(base==NSClassFromString(@"NativeOfflineMetalDevice"),319);
 actualTextureFactory=(TextureFactory)class_getMethodImplementation(base,@selector(newTextureWithDescriptor:));
 actualSurfaceFactory=(SurfaceFactory)class_getMethodImplementation(base,@selector(newTextureWithDescriptor:iosurface:plane:));
 actualBufferFactory=(BufferFactory)class_getMethodImplementation(base,@selector(newBufferWithLength:options:));
 actualPipelineFactory=(PipelineFactory)class_getMethodImplementation(base,@selector(newRenderPipelineStateWithDescriptor:error:));
 Class observed=objc_allocateClassPair(base,"OwnRenderBoxObservedMetalDevice",0);require(observed!=Nil,320);
 unsigned count=0;Method *methods=class_copyMethodList([OwnRenderBoxResourceObservation class],&count);require(count==4,321);
 for(unsigned i=0;i<count;i++) {
  Method actual=class_getInstanceMethod(base,method_getName(methods[i]));
  require(actual && !strcmp(method_getTypeEncoding(actual),method_getTypeEncoding(methods[i])) &&
   class_addMethod(observed,method_getName(methods[i]),method_getImplementation(methods[i]),method_getTypeEncoding(methods[i])),322);
 }
 free(methods);objc_registerClassPair(observed);require(class_getInstanceSize(observed)==class_getInstanceSize(base),323);object_setClass(device,observed);
 Class library=NSClassFromString(@"OMLibraryObject");SEL specialization=@selector(newFunctionWithName:constantValues:error:);
 if(library && !class_getInstanceMethod(library,specialization)) {
  Method diagnostic=class_getInstanceMethod([OwnFunctionConstantObservation class],specialization);
  require(diagnostic && class_addMethod(library,specialization,method_getImplementation(diagnostic),method_getTypeEncoding(diagnostic)),325);
 }
}
static void qualifyOriginalRenderBoxDrawing(id<MTLDevice> device,id rb) {
 phase=60;NSSetUncaughtExceptionHandler(observeUncaughtDrawingException);observeOwnResources(device);
 Class shapeClass=NSClassFromString(@"RBShape"),fillClass=NSClassFromString(@"RBFill");
 require(shapeClass && fillClass,310);
 for(NSArray *item in @[@[shapeClass,@"setRect:",@"v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"],
      @[fillClass,@"setColor:",@"v32@0:8{?=ffff}16"],
      @[NSClassFromString(@"RBDisplayList"),@"drawShape:fill:alpha:blendMode:",@"v40@0:8@16@24f32i36"]]) {
  Method method=class_getInstanceMethod(item[0],NSSelectorFromString(item[1]));
  require(method && !strcmp(method_getTypeEncoding(method),[item[2] UTF8String]),311);
 }
 id shape=[(id)shapeClass new],fill=[(id)fillClass new];require(shape && fill,312);
 CGRect bounds=CGRectMake(0,0,rbWidth,rbHeight);[shape setRect:bounds];[fill setColor:RBFloatColor{1,0,0,1}];
 id secondShape=[(id)shapeClass new],secondFill=[(id)fillClass new];require(secondShape && secondFill,324);
 [secondShape setRect:CGRectMake(rbWidth/2,0,rbWidth/2,rbHeight)];[secondFill setColor:RBFloatColor{0,0,1,1}];
 void *framework=dlopen("/System/Library/PrivateFrameworks/RenderBox.framework/RenderBox",RTLD_NOW|RTLD_LOCAL);require(framework!=nullptr,313);
 auto key=[&](const char *symbol)->NSString * {
  NSString *const *value=(NSString *const *)dlsym(framework,symbol);require(value && *value,314);return *value;
 };
 NSDictionary *options=@{key("RBImageRendererScale"):@1,key("RBImageRendererColorMode"):@0,key("RBImageRendererOpaque"):@YES};
 phase=61;CGImageRef image=[rb renderImageInRect:bounds options:options renderer:^(id list){
  phase=62;require([list isKindOfClass:NSClassFromString(@"RBDisplayList")],315);
  [list drawShape:shape fill:fill alpha:1 blendMode:kCGBlendModeCopy];
  [list drawShape:secondShape fill:secondFill alpha:1 blendMode:kCGBlendModeCopy];phase=63;
 }];
 phase=64;require(image && CGImageGetWidth(image)==rbWidth && CGImageGetHeight(image)==rbHeight,316);
 // No CPU draw call is used. Reading an actual returned image is diagnostic
 // validation only; the host GPU ledger must independently prove submission.
 CFDataRef pixels=CGDataProviderCopyData(CGImageGetDataProvider(image));
 require(pixels && CFDataGetLength(pixels)>=rbWidth*rbHeight*4,317);
 phase=66;size_t stride=CGImageGetBytesPerRow(image);CGBitmapInfo info=CGImageGetBitmapInfo(image);
 CGImageAlphaInfo alpha=CGImageGetAlphaInfo(image);unsigned order=info&kCGBitmapByteOrderMask;
 bool first=alpha==kCGImageAlphaPremultipliedFirst || alpha==kCGImageAlphaFirst || alpha==kCGImageAlphaNoneSkipFirst;
 bool last=alpha==kCGImageAlphaPremultipliedLast || alpha==kCGImageAlphaLast || alpha==kCGImageAlphaNoneSkipLast;
 // The actual original RGBA IOSurface image reports default order with
 // none-skip-last. Validate every returned byte as RGBA in that narrow case;
 // an incorrect interpretation still fails the full two-color image check.
 if(info==kCGImageAlphaNoneSkipLast)order=kCGBitmapByteOrder32Big;
 if(CGImageGetBitsPerComponent(image)!=8 || CGImageGetBitsPerPixel(image)!=32 || (!first && !last) ||
     (order!=kCGBitmapByteOrder32Little && order!=kCGBitmapByteOrder32Big) || stride<rbWidth*4 || stride>SIZE_MAX/rbHeight ||
     (size_t)CFDataGetLength(pixels)<stride*rbHeight)
  observeFailure([NSString stringWithFormat:@"Actual original image layout bpc %lu bpp %lu stride %lu bitmap 0x%x alpha %u",
   (unsigned long)CGImageGetBitsPerComponent(image),(unsigned long)CGImageGetBitsPerPixel(image),(unsigned long)stride,(unsigned)info,(unsigned)alpha]);
 const UInt8 *bytes=CFDataGetBytePtr(pixels);
 for(unsigned y=0;y<rbHeight;y++)for(unsigned x=0;x<rbWidth;x++) {
  const UInt8 *p=bytes+y*stride+x*4;unsigned r,g,b,a;
  if(order==kCGBitmapByteOrder32Little){r=p[first?2:3];g=p[first?1:2];b=p[first?0:1];a=p[first?3:0];}
  else {r=p[first?1:0];g=p[first?2:1];b=p[first?3:2];a=p[first?0:3];}
  if(r!=(x<rbWidth/2?255:0) || g || b!=(x<rbWidth/2?0:255) ||
      ((alpha!=kCGImageAlphaNoneSkipFirst && alpha!=kCGImageAlphaNoneSkipLast) && a!=255))
   observeFailure([NSString stringWithFormat:@"Actual original image pixel %u,%u rgba %u,%u,%u,%u bitmap 0x%x",x,y,r,g,b,a,(unsigned)info]);
 }
 CFRelease(pixels);CGImageRelease(image);require(!dlclose(framework),318);phase=67;
}
