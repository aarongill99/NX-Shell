#include <cstring>
#include <string>

// BMP
#include <libnsbmp.h>

// BMP
#include "libnsgif.h"

// JPEG
#include <turbojpeg.h>

// STB
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_BMP
#define STBI_NO_HDR
#define STBI_NO_JPEG
#define STBI_NO_PIC
#define STBI_NO_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_PNM
#define STBI_ONLY_PSD
#define STBI_ONLY_TGA
#include <stb_image.h>

// PNG
#include <png.h>

// WEBP
#include <webp/decode.h>

#include <switch.h>

#include "fs.h"
#include "gui.h"
#include "imgui.h"
#include "imgui_deko3d.h"
#include "log.h"
#include "textures.h"

#define BYTES_PER_PIXEL 4
#define MAX_IMAGE_BYTES (48 * 1024 * 1024)

Tex folder_icon, file_icons[NUM_FILE_ICONS], check_icon, uncheck_icon;

namespace BMP {
    static void *bitmap_create(int width, int height, [[maybe_unused]] unsigned int state) {
        /* ensure a stupidly large (>50Megs or so) bitmap is not created */
        if ((static_cast<long long>(width) * static_cast<long long>(height)) > (MAX_IMAGE_BYTES/BYTES_PER_PIXEL))
            return nullptr;
            
        return std::calloc(width * height, BYTES_PER_PIXEL);
    }
    
    static unsigned char *bitmap_get_buffer(void *bitmap) {
        assert(bitmap);
        return static_cast<unsigned char *>(bitmap);
    }
    
    static size_t bitmap_get_bpp([[maybe_unused]] void *bitmap) {
        return BYTES_PER_PIXEL;
    }
    
    static void bitmap_destroy(void *bitmap) {
        assert(bitmap);
        std::free(bitmap);
    }
}

namespace GIF {
    static void *bitmap_create(int width, int height) {
        /* ensure a stupidly large bitmap is not created */
        if ((static_cast<long long>(width) * static_cast<long long>(height)) > (MAX_IMAGE_BYTES/BYTES_PER_PIXEL))
            return nullptr;
            
        return std::calloc(width * height, BYTES_PER_PIXEL);
    }
    
    static void bitmap_set_opaque([[maybe_unused]] void *bitmap, [[maybe_unused]] bool opaque) {
        assert(bitmap);
    }
    
    static bool bitmap_test_opaque([[maybe_unused]] void *bitmap) {
        assert(bitmap);
        return false;
    }
    
    static unsigned char *bitmap_get_buffer(void *bitmap) {
        assert(bitmap);
        return static_cast<unsigned char *>(bitmap);
    }
    
    static void bitmap_destroy(void *bitmap) {
        assert(bitmap);
        std::free(bitmap);
    }
    
    static void bitmap_modified([[maybe_unused]] void *bitmap) {
        assert(bitmap);
        return;
    }
}

namespace Textures {
    typedef enum ImageType {
        ImageTypeBMP,
        ImageTypeGIF,
        ImageTypeJPEG,
        ImageTypePNG,
        ImageTypeWEBP,
        ImageTypeOther
    } ImageType;

    static u32 image_index = 1;
    
    static Result ReadFile(const char path[FS_MAX_PATH], unsigned char **buffer, s64 &size) {
        Result ret = 0;
        FsFile file;
        
        if (R_FAILED(ret = fsFsOpenFile(fs, path, FsOpenMode_Read, &file))) {
            Log::Error("fsFsOpenFile(%s) failed: 0x%x\n", path, ret);
            return ret;
        }
        
        if (R_FAILED(ret = fsFileGetSize(&file, &size))) {
            Log::Error("fsFileGetSize(%s) failed: 0x%x\n", path, ret);
            fsFileClose(&file);
            return ret;
        }

        *buffer = new unsigned char[size];

        u64 bytes_read = 0;
        if (R_FAILED(ret = fsFileRead(&file, 0, *buffer, static_cast<u64>(size), FsReadOption_None, &bytes_read))) {
            Log::Error("fsFileRead(%s) failed: 0x%x\n", path, ret);
            fsFileClose(&file);
            return ret;
        }
        
        if (bytes_read != static_cast<u64>(size)) {
            Log::Error("bytes_read(%llu) does not match file size(%llu)\n", bytes_read, size);
            fsFileClose(&file);
            return -1;
        }

        fsFileClose(&file);
        return 0;
    }

    static bool LoadImage(unsigned char *data, DkImageFormat format, Tex &texture) {
        int size = texture.width * texture.height * BYTES_PER_PIXEL;
        texture.image_id = image_index++;

        s_queue.waitIdle();
        
        dk::ImageLayout layout;
        dk::ImageLayoutMaker{s_device}
            .setFlags(0)
            .setFormat(format)
            .setDimensions(texture.width, texture.height)
            .initialize(layout);
            
        auto memBlock = dk::MemBlockMaker{s_device, imgui::deko3d::align(size, DK_MEMBLOCK_ALIGNMENT)}
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();
        
        s_imageMemBlock = dk::MemBlockMaker{s_device, imgui::deko3d::align(layout.getSize(), DK_MEMBLOCK_ALIGNMENT)}
            .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
            .create();

        std::memcpy(memBlock.getCpuAddr(), data, size);

        dk::Image image;
        image.initialize(layout, s_imageMemBlock, 0);
        s_imageDescriptors[texture.image_id].initialize(image);
        
        dk::ImageView imageView(image);
        
        s_cmdBuf[0].copyBufferToImage({memBlock.getGpuAddr()}, imageView,
            {0, 0, 0, static_cast<std::uint32_t>(texture.width), static_cast<std::uint32_t>(texture.height), 1});
        
        s_queue.submitCommands(s_cmdBuf[0].finishList());
        
        s_samplerDescriptors[texture.sampler_id].initialize(dk::Sampler{}
            .setFilter(DkFilter_Linear, DkFilter_Linear)
            .setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge));

        s_queue.waitIdle();
        return true;
    }
    
    static bool LoadImageRomfs(const std::string &path, Tex &texture) {
        bool ret = false;
        png_image image;
        std::memset(&image, 0, (sizeof image));
        image.version = PNG_IMAGE_VERSION;

        if (png_image_begin_read_from_file(&image, path.c_str()) != 0) {
            png_bytep buffer;
            image.format = PNG_FORMAT_RGBA;
            buffer = new png_byte[PNG_IMAGE_SIZE(image)];

            if (buffer != nullptr && png_image_finish_read(&image, nullptr, buffer, 0, nullptr) != 0) {
                texture.width = image.width;
                texture.height = image.height;
                ret = Textures::LoadImage(buffer, DkImageFormat_RGBA8_Unorm, texture);
                delete[] buffer;
                png_image_free(&image);
            }
            else {
                if (buffer == nullptr)
                    png_image_free(&image);
                else
                    delete[] buffer;
            }
        }

        return ret;
    }
    
    static bool LoadImageBMP(unsigned char **data, s64 &size, Tex &texture) {
        bmp_bitmap_callback_vt bitmap_callbacks = {
            BMP::bitmap_create,
            BMP::bitmap_destroy,
            BMP::bitmap_get_buffer,
            BMP::bitmap_get_bpp
        };
        
        bmp_result code = BMP_OK;
        bmp_image bmp;
        bmp_create(&bmp, &bitmap_callbacks);
        
        code = bmp_analyse(&bmp, size, *data);
        if (code != BMP_OK) {
            bmp_finalise(&bmp);
            return false;
        }
        
        code = bmp_decode(&bmp);
        if (code != BMP_OK) {
            if ((code != BMP_INSUFFICIENT_DATA) && (code != BMP_DATA_ERROR)) {
                bmp_finalise(&bmp);
                return false;
            }
            
            /* skip if the decoded image would be ridiculously large */
            if ((bmp.width * bmp.height) > 200000) {
                bmp_finalise(&bmp);
                return false;
            }
        }
        
        texture.width = bmp.width;
        texture.height = bmp.height;
        bool ret = LoadImage(static_cast<unsigned char *>(bmp.bitmap), DkImageFormat_RGBA8_Unorm, texture);
        bmp_finalise(&bmp);
        return ret;
    }

    static bool LoadImageGIF(unsigned char **data, s64 &size, std::vector<Tex> &textures) {
        gif_bitmap_callback_vt bitmap_callbacks = {
            GIF::bitmap_create,
            GIF::bitmap_destroy,
            GIF::bitmap_get_buffer,
            GIF::bitmap_set_opaque,
            GIF::bitmap_test_opaque,
            GIF::bitmap_modified
        };
        
        bool ret = false;
        gif_animation gif;
        gif_result code = GIF_OK;
        gif_create(&gif, &bitmap_callbacks);
        
        do {
            code = gif_initialise(&gif, size, *data);
            if (code != GIF_OK && code != GIF_WORKING) {
                Log::Error("gif_initialise failed: %d\n", code);
                gif_finalise(&gif);
                return ret;
            }
        } while (code != GIF_OK);
        
        bool gif_is_animated = gif.frame_count > 1;
        
        if (gif_is_animated) {
            textures.resize(gif.frame_count);
            
            for (unsigned int i = 0; i < gif.frame_count; i++) {
                code = gif_decode_frame(&gif, i);
                if (code != GIF_OK) {
                    Log::Error("gif_decode_frame failed: %d\n", code);
                    return false;
                }
                
                textures[i].width = gif.width;
                textures[i].height = gif.height;
                textures[i].delay = gif.frames->frame_delay;
                ret = Textures::LoadImage(static_cast<unsigned char *>(gif.frame_image), DkImageFormat_RGBA8_Unorm, textures[i]);
            }
        }
        else {
            code = gif_decode_frame(&gif, 0);
            if (code != GIF_OK) {
                Log::Error("gif_decode_frame failed: %d\n", code);
                return false;
            }
            
            textures[0].width = gif.width;
            textures[0].height = gif.height;
            ret = Textures::LoadImage(static_cast<unsigned char *>(gif.frame_image), DkImageFormat_RGBA8_Unorm, textures[0]);
        }
        
        gif_finalise(&gif);
        return ret;
    }
    
    static bool LoadImageJPEG(unsigned char **data, s64 &size, Tex &texture) {
        tjhandle jpeg = tjInitDecompress();
        int jpegsubsamp = 0;
        tjDecompressHeader2(jpeg, *data, size, &texture.width, &texture.height, &jpegsubsamp);
        unsigned char *buffer = new unsigned char[texture.width * texture.height * 4];
        tjDecompress2(jpeg, *data, size, buffer, texture.width, 0, texture.height, TJPF_RGBA, TJFLAG_FASTDCT);
        bool ret = LoadImage(buffer, DkImageFormat_RGBA8_Unorm, texture);
        tjDestroy(jpeg);
        delete[] buffer;
        return ret;
    }

    static bool LoadImageOther(unsigned char **data, s64 &size, Tex &texture) {
        unsigned char *image = stbi_load_from_memory(*data, size, &texture.width, &texture.height, nullptr, STBI_rgb_alpha);
        bool ret = Textures::LoadImage(image, DkImageFormat_RGBA8_Unorm, texture);
        return ret;
    }

    static bool LoadImagePNG(unsigned char **data, s64 &size, Tex &texture) {
        bool ret = false;
        png_image image;
        std::memset(&image, 0, (sizeof image));
        image.version = PNG_IMAGE_VERSION;

        if (png_image_begin_read_from_memory(&image, *data, size) != 0) {
            png_bytep buffer;
            image.format = PNG_FORMAT_RGBA;
            buffer = new png_byte[PNG_IMAGE_SIZE(image)];

            if (buffer != nullptr && png_image_finish_read(&image, nullptr, buffer, 0, nullptr) != 0) {
                texture.width = image.width;
                texture.height = image.height;
                ret = Textures::LoadImage(buffer, DkImageFormat_RGBA8_Unorm, texture);
                delete[] buffer;
                png_image_free(&image);
            }
            else {
                if (buffer == nullptr)
                    png_image_free(&image);
                else
                    delete[] buffer;
            }
        }

        return ret;
    }

    static bool LoadImageWEBP(unsigned char **data, s64 &size, Tex &texture) {
        *data = WebPDecodeRGBA(*data, size, &texture.width, &texture.height);
        bool ret = Textures::LoadImage(*data, DkImageFormat_RGBA8_Unorm, texture);
        return ret;
    }

    ImageType GetImageType(const std::string &filename) {
        std::string ext = FS::GetFileExt(filename);
        
        if (!ext.compare(".BMP"))
            return ImageTypeBMP;
        else if (!ext.compare(".GIF"))
            return ImageTypeGIF;
        else if ((!ext.compare(".JPG")) || (!ext.compare(".JPEG")))
            return ImageTypeJPEG;
        else if (!ext.compare(".PNG"))
            return ImageTypePNG;
        else if (!ext.compare(".WEBP"))
            return ImageTypeWEBP;
        
        return ImageTypeOther;
    }

    bool LoadImageFile(const char path[FS_MAX_PATH], std::vector<Tex> &textures) {
        bool ret = false;
        unsigned char *data = nullptr;
        s64 size = 0;

        if (R_FAILED(Textures::ReadFile(path, &data, size))) {
            delete[] data;
            return ret;
        }

        // Resize to 1 initially. If the file is a GIF it will be resized accordingly.
        textures.resize(1);

        ImageType type = GetImageType(path);
        switch(type) {
            case ImageTypeBMP:
                ret = Textures::LoadImageBMP(&data, size, textures[0]);
                break;

            case ImageTypeGIF:
                ret = Textures::LoadImageGIF(&data, size, textures);
                break;
            
            case ImageTypeJPEG:
                ret = Textures::LoadImageJPEG(&data, size, textures[0]);
                break;

            case ImageTypePNG:
                ret = Textures::LoadImagePNG(&data, size, textures[0]);
                break;

            case ImageTypeWEBP:
                ret = Textures::LoadImageWEBP(&data, size, textures[0]);
                break;

            default:
                ret = Textures::LoadImageOther(&data, size, textures[0]);
                break;
        }

        delete[] data;
        return ret;
    }
    
    void Init(void) {
        const std::string paths[NUM_FILE_ICONS] {
            "romfs:/file.png",
            "romfs:/archive.png",
            "romfs:/image.png",
            "romfs:/text.png"
        };

        bool image_ret = Textures::LoadImageRomfs("romfs:/folder.png", folder_icon);
        IM_ASSERT(image_ret);

        image_ret = Textures::LoadImageRomfs("romfs:/check.png", check_icon);
        IM_ASSERT(image_ret);

        image_ret = Textures::LoadImageRomfs("romfs:/uncheck.png", uncheck_icon);
        IM_ASSERT(image_ret);
        
        for (int i = 0; i < NUM_FILE_ICONS; i++) {
            bool ret = Textures::LoadImageRomfs(paths[i], file_icons[i]);
            IM_ASSERT(ret);
        }
    }
    
    void Free(Tex *texture) {
        //glDeleteTextures(1, &texture->id);
    }
    
    void Exit(void) {
        // for (int i = 0; i < NUM_FILE_ICONS; i++)
        // 	glDeleteTextures(1, &file_icons[i].id);
        
        // glDeleteTextures(1, &uncheck_icon.id);
        // glDeleteTextures(1, &check_icon.id);
        // glDeleteTextures(1, &folder_icon.id);
    }
}
