#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static int expected(int fn, float a, float r) {
    switch(fn) { case 1:return 0; case 2:return a<r; case 3:return a==r; case 4:return a<=r; case 5:return a>r; case 6:return a!=r; case 7:return a>=r; default:return 1; }
}
int main(int argc, const char **argv) { @autoreleasepool {
    if(argc!=2) return 2;
    id<MTLDevice> dev=MTLCreateSystemDefaultDevice(); if(!dev) { fprintf(stderr,"No Metal device\n"); return 2; }
    NSError *err=nil;
    NSString *src=[NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&err];
    id<MTLLibrary> lib=[dev newLibraryWithSource:src options:nil error:&err];
    if(!lib) { fprintf(stderr,"%s\n",err.description.UTF8String); return 2; }
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
    pd.vertexFunction=[lib newFunctionWithName:@"vtx"]; pd.fragmentFunction=[lib newFunctionWithName:@"main0"];
    pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    pd.colorAttachments[0].writeMask=MTLColorWriteMaskNone;
    pd.depthAttachmentPixelFormat=pd.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
    id<MTLRenderPipelineState> pipe=[dev newRenderPipelineStateWithDescriptor:pd error:&err];
    id<MTLComputePipelineState> reader=[dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"read_planes"] error:&err];
    if(!pipe||!reader) { fprintf(stderr,"%s\n",err.description.UTF8String); return 2; }
    MTLDepthStencilDescriptor *dd=[MTLDepthStencilDescriptor new]; dd.depthCompareFunction=MTLCompareFunctionAlways; dd.depthWriteEnabled=YES;
    MTLStencilDescriptor *sd=[MTLStencilDescriptor new]; sd.stencilCompareFunction=MTLCompareFunctionAlways; sd.depthStencilPassOperation=MTLStencilOperationReplace;
    dd.frontFaceStencil=dd.backFaceStencil=sd; id<MTLDepthStencilState> ds=[dev newDepthStencilStateWithDescriptor:dd];
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:4 height:1 mipmapped:NO];
    td.storageMode=MTLStorageModePrivate; td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
    id<MTLTexture> depth=[dev newTextureWithDescriptor:td]; id<MTLTexture> stencil=[depth newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
    td.pixelFormat=MTLPixelFormatRGBA8Unorm; id<MTLTexture> color=[dev newTextureWithDescriptor:td];
    id<MTLBuffer> output=[dev newBufferWithLength:32 options:MTLResourceStorageModeShared]; id<MTLCommandQueue> q=[dev newCommandQueue];
    int checks=0, fails=0; float refs[]={0,.25f,.498039f,.5f,1}, alphas[]={0,.25f,.5f,1};
    for(int fn=1;fn<=8;fn++) for(int ri=0;ri<5;ri++) {
        MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture=color; rp.colorAttachments[0].loadAction=MTLLoadActionClear; rp.colorAttachments[0].storeAction=MTLStoreActionStore;
        rp.depthAttachment.texture=rp.stencilAttachment.texture=depth;
        rp.depthAttachment.loadAction=rp.stencilAttachment.loadAction=MTLLoadActionClear;
        rp.depthAttachment.storeAction=rp.stencilAttachment.storeAction=MTLStoreActionStore;
        rp.depthAttachment.clearDepth=1; rp.stencilAttachment.clearStencil=0;
        id<MTLCommandBuffer> cb=[q commandBuffer]; id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:pipe]; [e setDepthStencilState:ds]; [e setStencilReferenceValue:7];
        struct { uint32_t function; float reference; } state={fn,refs[ri]};
        [e setFragmentBytes:&state length:sizeof state atIndex:15]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding];
        id<MTLComputeCommandEncoder> c=[cb computeCommandEncoder]; [c setComputePipelineState:reader];
        [c setTexture:stencil atIndex:0]; [c setTexture:depth atIndex:1]; [c setBuffer:output offset:0 atIndex:0];
        [c dispatchThreads:MTLSizeMake(4,1,1) threadsPerThreadgroup:MTLSizeMake(4,1,1)]; [c endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if(cb.status!=MTLCommandBufferStatusCompleted) {fprintf(stderr,"GPU error %s\n",cb.error.description.UTF8String);return 2;}
        uint32_t *v=output.contents;
        for(int i=0;i<4;i++) { int pass=expected(fn,alphas[i],refs[ri]); float z; memcpy(&z,&v[2*i+1],4); checks+=2;
            if(v[2*i]!=(pass?7:0)) fails++;
            if(fabsf(z-(pass?.25f:1.f))>0.00001f) fails++;
        }
    }
    printf("alpha rejection: %d depth/stencil checks, %d failures\n",checks,fails); return fails?1:0;
}}
