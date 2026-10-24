// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2025 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include <android/asset_manager_jni.h>
#include <android/native_window_jni.h>
#include <android/native_window.h>

#include <android/log.h>

#include <jni.h>

#include <string>
#include <string.h>
#include <vector>
#include <mutex>

#include <platform.h>
#include <benchmark.h>

#include "rvm.h"

#include "ndkcamera.h"

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#if __ARM_NEON
#include <arm_neon.h>
#endif // __ARM_NEON

static RVM* g_rvm = 0;
static ncnn::Mutex lock;
static InterFeatures g_feats;
static float g_current_fps = 0.0f;
static ANativeWindow* g_encoder_win = 0;
static std::mutex g_encoder_mutex;

static int draw_unsupported(cv::Mat& rgb)
{
    const char text[] = "unsupported";

    int baseLine = 0;
    cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 1.0, 1, &baseLine);

    int y = (rgb.rows - label_size.height) / 2;
    int x = (rgb.cols - label_size.width) / 2;

    cv::rectangle(rgb, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)),
                    cv::Scalar(255, 255, 255), -1);

    cv::putText(rgb, text, cv::Point(x, y + label_size.height),
                cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 0));

    return 0;
}

static int update_fps()
{
    static double t0 = 0.f;
    static float fps_history[10] = {0.f};

    double t1 = ncnn::get_current_time();
    if (t0 == 0.f)
    {
        t0 = t1;
        return 0;
    }

    float fps = 1000.f / (t1 - t0);
    t0 = t1;

    for (int i = 9; i >= 1; i--)
    {
        fps_history[i] = fps_history[i - 1];
    }
    fps_history[0] = fps;

    if (fps_history[9] == 0.f)
    {
        return 0;
    }

    float avg_fps = 0.f;
    for (int i = 0; i < 10; i++)
    {
        avg_fps += fps_history[i];
    }
    g_current_fps = avg_fps / 10.f;

    return 0;
}

static int render_to_encoder_window(const cv::Mat& rgb)
{
    std::lock_guard<std::mutex> guard(g_encoder_mutex);
    if (!g_encoder_win) return 0;

    int render_w = rgb.cols;
    int render_h = rgb.rows;

    if (ANativeWindow_setBuffersGeometry(g_encoder_win, render_w, render_h, WINDOW_FORMAT_RGBA_8888) != 0)
        return -1;

    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(g_encoder_win, &buf, NULL) != 0)
        return -1;

    if ((buf.format == WINDOW_FORMAT_RGBA_8888 || buf.format == WINDOW_FORMAT_RGBX_8888)
        && buf.width >= render_w && buf.height >= render_h)
    {
        for (int y = 0; y < render_h; y++)
        {
            const unsigned char* ptr = rgb.ptr<const unsigned char>(y);
            unsigned char* outptr = (unsigned char*)buf.bits + buf.stride * 4 * y;

            int x = 0;
#if __ARM_NEON
            for (; x + 7 < render_w; x += 8)
            {
                uint8x8x3_t _rgb = vld3_u8(ptr);
                uint8x8x4_t _rgba;
                _rgba.val[0] = _rgb.val[0];
                _rgba.val[1] = _rgb.val[1];
                _rgba.val[2] = _rgb.val[2];
                _rgba.val[3] = vdup_n_u8(255);
                vst4_u8(outptr, _rgba);

                ptr += 24;
                outptr += 32;
            }
#endif // __ARM_NEON
            for (; x < render_w; x++)
            {
                outptr[0] = ptr[0];
                outptr[1] = ptr[1];
                outptr[2] = ptr[2];
                outptr[3] = 255;

                ptr += 3;
                outptr += 4;
            }
        }
    }

    ANativeWindow_unlockAndPost(g_encoder_win);
    return 0;
}

class MyNdkCamera : public NdkCameraWindow
{
public:
    virtual void on_image_render(cv::Mat& rgb) const;
};

void MyNdkCamera::on_image_render(cv::Mat& rgb) const
{
    {
        ncnn::MutexLockGuard g(lock);

        if (g_rvm)
        {
            cv::Mat fgr;
            cv::Mat pha;
            cv::Mat seg;
            g_rvm->detect(rgb, g_feats, fgr, pha, seg);

            if (!fgr.empty() && !pha.empty())
            {
                // Post-process alpha matte
                g_rvm->postprocess_alpha(pha);

                // Composite onto solid magenta background (#FF00FF)
                g_rvm->draw_magenta(rgb, fgr, pha);
            }
            else
            {
                g_rvm->draw(rgb, fgr, pha, seg);
            }
        }
        else
        {
            draw_unsupported(rgb);
        }
    }

    update_fps();

    // Direct render to MediaCodec hardware encoder input Surface if attached
    render_to_encoder_window(rgb);
}

static MyNdkCamera* g_camera = 0;

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_DEBUG, "FlipCam", "JNI_OnLoad");

    if (NdkCamera::initialize(vm) != 0) return JNI_ERR;

    g_camera = new MyNdkCamera;

    ncnn::create_gpu_instance();

    return JNI_VERSION_1_4;
}

JNIEXPORT void JNI_OnUnload(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_DEBUG, "FlipCam", "JNI_OnUnload");

    g_camera->close();

    {
        ncnn::MutexLockGuard g(lock);

        delete g_rvm;
        g_rvm = 0;
    }

    {
        std::lock_guard<std::mutex> guard(g_encoder_mutex);
        if (g_encoder_win)
        {
            ANativeWindow_release(g_encoder_win);
            g_encoder_win = 0;
        }
    }

    ncnn::destroy_gpu_instance();

    delete g_camera;
    g_camera = 0;
    NdkCamera::uninitialize(vm);
}

JNIEXPORT jboolean JNICALL Java_com_tencent_rvmncnn_RVMNcnn_loadModel(JNIEnv* env, jobject thiz, jobject assetManager, jint modelid, jint sizeid, jint intrainterid, jint postprocid, jint cpugpu)
{
    if (modelid < 0 || modelid > 1 || sizeid < 0 || sizeid > 6 || intrainterid < 0 || intrainterid > 1 || postprocid < 0 || postprocid > 2 || cpugpu < 0 || cpugpu > 3)
    {
        return JNI_FALSE;
    }

    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);

    __android_log_print(ANDROID_LOG_DEBUG, "FlipCam", "loadModel %p, modelid=%d, sizeid=%d, cpugpu=%d", mgr, modelid, sizeid, cpugpu);

    const char* modeltypes[2] =
    {
        "mobilenetv3",
        "resnet50"
    };

    const int sizetypes[7] =
    {
        256,
        320,
        384,
        448,
        512,
        576,
        640
    };

    std::string parampath = std::string("rvm_") + modeltypes[(int)modelid] + ".ncnn.param";
    std::string modelpath = std::string("rvm_") + modeltypes[(int)modelid] + ".ncnn.bin";

    // Enforcement according to spec:
    // MobileNetV3 (default): CPU mode
    // ResNet50 (quality): Vulkan GPU mode
    bool use_gpu = (modelid == 1) || (cpugpu == 1);
    bool use_turnip = (cpugpu == 2);
    bool use_momoten = (cpugpu == 3);

    {
        ncnn::MutexLockGuard g(lock);

        {
            // reset inter feats
            g_feats.r1.release();
            g_feats.r2.release();
            g_feats.r3.release();
            g_feats.r4.release();

            static int old_modelid = -1;
            static int old_cpugpu = -1;

            if (modelid != old_modelid || cpugpu != old_cpugpu)
            {
                delete g_rvm;
                g_rvm = 0;
            }
            old_modelid = modelid;

            if (cpugpu != old_cpugpu)
            {
                ncnn::destroy_gpu_instance();
                old_cpugpu = -1;

                if (use_momoten)
                {
                    if (ncnn::create_gpu_instance("libmomoten.so") != 0)
                    {
                        ncnn::destroy_gpu_instance();
                        return JNI_FALSE;
                    }
                }
                else if (use_turnip)
                {
                    if (ncnn::create_gpu_instance("libvulkan_freedreno.so") != 0)
                    {
                        ncnn::destroy_gpu_instance();
                        return JNI_FALSE;
                    }
                }
                else if (use_gpu)
                {
                    ncnn::create_gpu_instance();
                }
                old_cpugpu = cpugpu;
            }

            if (!g_rvm)
            {
                g_rvm = new RVM;
                g_rvm->load(mgr, parampath.c_str(), modelpath.c_str(), use_gpu || use_turnip || use_momoten);
            }
            g_rvm->set_model_type((int)modelid);
            g_rvm->set_target_size(sizetypes[(int)sizeid]);
            g_rvm->set_intra_inter((int)intrainterid);

            if (postprocid == 0)
                g_rvm->set_postproc_mode(false, true, false);
            if (postprocid == 1)
                g_rvm->set_postproc_mode(false, false, true);
            if (postprocid == 2)
                g_rvm->set_postproc_mode(true, false, false);
        }
    }

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_com_tencent_rvmncnn_RVMNcnn_openCamera(JNIEnv* env, jobject thiz, jint facing)
{
    if (facing < -1 || facing > 1)
        return JNI_FALSE;

    return g_camera->open((int)facing) == 0;
}

JNIEXPORT jboolean JNICALL Java_com_tencent_rvmncnn_RVMNcnn_switchCamera(JNIEnv* env, jobject thiz)
{
    return g_camera->switch_camera() == 0;
}

JNIEXPORT jboolean JNICALL Java_com_tencent_rvmncnn_RVMNcnn_closeCamera(JNIEnv* env, jobject thiz)
{
    g_camera->close();
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_com_tencent_rvmncnn_RVMNcnn_setCameraActive(JNIEnv* env, jobject thiz, jboolean active)
{
    return g_camera->set_active(active) == 0;
}

JNIEXPORT jboolean JNICALL Java_com_tencent_rvmncnn_RVMNcnn_setCameraPermission(JNIEnv* env, jobject thiz, jboolean granted)
{
    return g_camera->set_permission(granted) == 0;
}

JNIEXPORT jboolean JNICALL Java_com_tencent_rvmncnn_RVMNcnn_setOutputWindow(JNIEnv* env, jobject thiz, jobject surface)
{
    return g_camera->set_surface(env, surface) == 0;
}

JNIEXPORT jboolean JNICALL Java_com_tencent_rvmncnn_RVMNcnn_setEncoderWindow(JNIEnv* env, jobject thiz, jobject surface)
{
    std::lock_guard<std::mutex> guard(g_encoder_mutex);
    if (g_encoder_win)
    {
        ANativeWindow_release(g_encoder_win);
        g_encoder_win = 0;
    }

    if (surface)
    {
        g_encoder_win = ANativeWindow_fromSurface(env, surface);
    }
    return JNI_TRUE;
}

JNIEXPORT jfloat JNICALL Java_com_tencent_rvmncnn_RVMNcnn_getCurrentFps(JNIEnv* env, jobject thiz)
{
    return g_current_fps;
}

JNIEXPORT jfloat JNICALL Java_com_tencent_rvmncnn_RVMNcnn_runBenchmark(JNIEnv* env, jobject thiz, jobject assetManager, jint modelid, jint target_size, jint num_frames)
{
    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);
    if (!mgr) return 0.0f;

    const char* modeltypes[2] = { "mobilenetv3", "resnet50" };
    std::string parampath = std::string("rvm_") + modeltypes[modelid % 2] + ".ncnn.param";
    std::string modelpath = std::string("rvm_") + modeltypes[modelid % 2] + ".ncnn.bin";

    RVM bench_rvm;
    bool use_gpu = (modelid == 1);
    bench_rvm.load(mgr, parampath.c_str(), modelpath.c_str(), use_gpu);
    bench_rvm.set_model_type(modelid);
    bench_rvm.set_target_size(target_size);
    bench_rvm.set_intra_inter(1);

    cv::Mat dummy_rgb(480, 640, CV_8UC3, cv::Scalar(128, 128, 128));
    InterFeatures feats;
    cv::Mat fgr, pha, seg;

    // Warmup frame
    bench_rvm.detect(dummy_rgb, feats, fgr, pha, seg);

    double start = ncnn::get_current_time();
    int count = (num_frames > 0) ? num_frames : 15;
    for (int i = 0; i < count; i++)
    {
        bench_rvm.detect(dummy_rgb, feats, fgr, pha, seg);
    }
    double elapsed = ncnn::get_current_time() - start;

    if (elapsed <= 0) return 30.0f;
    return (float)(count * 1000.0 / elapsed);
}

}
