/* The iOS app's icon from the Mac's (tools/build_xiv_ios.py): a picture (the .icns build_xiv.py makes
 * from the player's ffxivboot.exe) cropped to what is not transparent, drawn on opaque white and
 * scaled to the 1024x1024 PNG an asset catalog takes. iOS masks it to its own shape and would fill
 * transparent corners black.
 *   ios_icon <picture> <out.png> */
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: ios_icon <picture> <out.png>\n");
        return 2;
    }
    @autoreleasepool
    {
        NSURL* in = [NSURL fileURLWithPath:@(argv[1])];
        CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)in, NULL);
        if (!src)
            return fprintf(stderr, "ios_icon: cannot read %s\n", argv[1]), 1;
        /* the largest picture in it (an .icns holds several) */
        size_t best = 0, best_w = 0;
        for (size_t i = 0; i < CGImageSourceGetCount(src); ++i)
        {
            CGImageRef im = CGImageSourceCreateImageAtIndex(src, i, NULL);
            if (im && CGImageGetWidth(im) > best_w)
                best = i, best_w = CGImageGetWidth(im);
            if (im)
                CGImageRelease(im);
        }
        CGImageRef im = CGImageSourceCreateImageAtIndex(src, best, NULL);
        CFRelease(src);
        if (!im)
            return fprintf(stderr, "ios_icon: no picture in %s\n", argv[1]), 1;
        size_t w = CGImageGetWidth(im), h = CGImageGetHeight(im);
        /* its pixels as RGBA, to find the box of what is not transparent */
        uint8_t* px = calloc(w * h, 4);
        CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
        CGContextRef c = CGBitmapContextCreate(px, w, h, 8, w * 4, rgb, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
        CGContextDrawImage(c, CGRectMake(0, 0, w, h), im);
        size_t x0 = w, y0 = h, x1 = 0, y1 = 0;
        for (size_t y = 0; y < h; ++y)
            for (size_t x = 0; x < w; ++x)
                if (px[(y * w + x) * 4 + 3] > 200)
                {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        CGContextRelease(c);
        free(px);
        if (x1 < x0)
            x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1;
        /* bitmap rows run top down, CGImage crops too */
        CGImageRef crop = CGImageCreateWithImageInRect(im, CGRectMake(x0, y0, x1 - x0 + 1, y1 - y0 + 1));
        const size_t N = 1024;
        CGContextRef out = CGBitmapContextCreate(NULL, N, N, 8, N * 4, rgb, (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
        CGContextSetRGBFillColor(out, 1, 1, 1, 1);
        CGContextFillRect(out, CGRectMake(0, 0, N, N));
        CGContextSetInterpolationQuality(out, kCGInterpolationHigh);
        CGContextDrawImage(out, CGRectMake(0, 0, N, N), crop);
        CGImageRef done = CGBitmapContextCreateImage(out);
        NSURL* to = [NSURL fileURLWithPath:@(argv[2])];
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)to, CFSTR("public.png"), 1, NULL);
        CGImageDestinationAddImage(dst, done, NULL);
        int ok = CGImageDestinationFinalize(dst);
        CFRelease(dst), CGImageRelease(done), CGContextRelease(out), CGImageRelease(crop), CGImageRelease(im);
        CGColorSpaceRelease(rgb);
        if (!ok)
            return fprintf(stderr, "ios_icon: cannot write %s\n", argv[2]), 1;
        printf("ios_icon: %s from %zux%zu (box %zu,%zu-%zu,%zu)\n", argv[2], w, h, x0, y0, x1, y1);
    }
    return 0;
}
