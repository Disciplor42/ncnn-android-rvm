package com.tencent.rvmncnn;

import android.Manifest;
import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.Button;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

public class MainActivity extends Activity implements SurfaceHolder.Callback
{
    private static final String TAG = "FlipCam_Main";
    public static final int REQUEST_PERMISSIONS = 100;

    private RVMNcnn rvmncnn = new RVMNcnn();

    private Spinner spinnerModel;
    private Spinner spinnerResolution;
    private Button buttonStartStop;
    private Button buttonSwitchCamera;
    private TextView textStatus;
    private TextView textDetection;
    private TextView textFps;

    private int currentModel = 0;      // 0: Fast (MobileNetV3), 1: Quality (ResNet50)
    private int currentResolution = 0; // 0: Auto, 1: 1080p, 2: 720p, 3: 540p
    private int actualWidth = 1280;
    private int actualHeight = 720;
    private int actualTargetSize = 512;
    private int actualBitrate = 6000000;

    private boolean isStreaming = false;
    private TcpServerEncoder encoder;
    private StreamingService streamingService;
    private boolean isServiceBound = false;

    private Handler fpsHandler = new Handler(Looper.getMainLooper());
    private Runnable fpsRunnable;

    private ServiceConnection serviceConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            StreamingService.LocalBinder binder = (StreamingService.LocalBinder) service;
            streamingService = binder.getService();
            isServiceBound = true;
            if (encoder != null) {
                streamingService.setEncoder(encoder);
            }
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            streamingService = null;
            isServiceBound = false;
        }
    };

    @Override
    public void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.main);

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        SurfaceView cameraView = findViewById(R.id.cameraview);
        cameraView.getHolder().setFormat(PixelFormat.RGBA_8888);
        cameraView.getHolder().addCallback(this);

        textStatus = findViewById(R.id.textStatus);
        textDetection = findViewById(R.id.textDetection);
        textFps = findViewById(R.id.textFps);

        buttonSwitchCamera = findViewById(R.id.buttonSwitchCamera);
        buttonSwitchCamera.setOnClickListener(v -> rvmncnn.switchCamera());

        buttonStartStop = findViewById(R.id.buttonStartStop);
        buttonStartStop.setOnClickListener(v -> {
            if (isStreaming) {
                stopStreaming();
            } else {
                startStreaming();
            }
        });

        spinnerModel = findViewById(R.id.spinnerModel);
        spinnerModel.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (position != currentModel) {
                    currentModel = position;
                    reload();
                }
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });

        spinnerResolution = findViewById(R.id.spinnerResolution);
        spinnerResolution.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (position != currentResolution) {
                    currentResolution = position;
                    reload();
                }
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });

        requestAppPermissions();
        reload();
        startFpsMonitor();
    }

    private void requestAppPermissions() {
        String[] permissions;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            permissions = new String[]{Manifest.permission.CAMERA, Manifest.permission.POST_NOTIFICATIONS};
        } else {
            permissions = new String[]{Manifest.permission.CAMERA};
        }

        boolean allGranted = true;
        for (String p : permissions) {
            if (ContextCompat.checkSelfPermission(this, p) != PackageManager.PERMISSION_GRANTED) {
                allGranted = false;
                break;
            }
        }

        if (!allGranted) {
            ActivityCompat.requestPermissions(this, permissions, REQUEST_PERMISSIONS);
        } else {
            rvmncnn.setCameraPermission(true);
        }
    }

    private void reload() {
        // Compute resolution parameters
        if (currentResolution == 0) {
            // Auto-detect performance
            runAutoPerformanceDetection();
        } else if (currentResolution == 1) { // 1080p
            actualWidth = 1920;
            actualHeight = 1080;
            actualTargetSize = 640;
            actualBitrate = 10000000;
            textDetection.setText("Manual resolution: 1080p");
        } else if (currentResolution == 2) { // 720p
            actualWidth = 1280;
            actualHeight = 720;
            actualTargetSize = 512;
            actualBitrate = 6000000;
            textDetection.setText("Manual resolution: 720p");
        } else { // 540p
            actualWidth = 960;
            actualHeight = 540;
            actualTargetSize = 384;
            actualBitrate = 4000000;
            textDetection.setText("Manual resolution: 540p");
        }

        // Enforcement according to spec:
        // Model 0 (Fast): MobileNetV3 -> CPU (cpugpu=0)
        // Model 1 (Quality): ResNet50 -> Vulkan (cpugpu=1)
        int cpugpu = (currentModel == 1) ? 1 : 0;
        int sizeid = 4; // 512 default target size
        if (actualTargetSize == 640) sizeid = 6;
        if (actualTargetSize == 384) sizeid = 2;

        boolean ok = rvmncnn.loadModel(getAssets(), currentModel, sizeid, 1, 0, cpugpu);
        if (!ok) {
            Log.e(TAG, "rvmncnn loadModel failed");
        }

        if (isStreaming) {
            // Restart encoder with updated dimensions if streaming
            stopStreaming();
            startStreaming();
        }
    }

    private void runAutoPerformanceDetection() {
        // Benchmark 1080p candidate
        float fps1080 = rvmncnn.runBenchmark(getAssets(), currentModel, 640, 10);
        if (fps1080 >= 15.0f) {
            actualWidth = 1920;
            actualHeight = 1080;
            actualTargetSize = 640;
            actualBitrate = 10000000;
            textDetection.setText(String.format("Auto-detected: 1080p (%.1f FPS)", fps1080));
            return;
        }

        // Benchmark 720p candidate
        float fps720 = rvmncnn.runBenchmark(getAssets(), currentModel, 512, 10);
        if (fps720 >= 15.0f) {
            actualWidth = 1280;
            actualHeight = 720;
            actualTargetSize = 512;
            actualBitrate = 6000000;
            textDetection.setText(String.format("Auto-detected: 720p (%.1f FPS)", fps720));
            return;
        }

        // Benchmark 540p candidate
        float fps540 = rvmncnn.runBenchmark(getAssets(), currentModel, 384, 10);
        actualWidth = 960;
        actualHeight = 540;
        actualTargetSize = 384;
        actualBitrate = 4000000;

        if (fps540 < 10.0f) {
            textDetection.setText(String.format("Auto: 540p (%.1f FPS) - Warning: slow phone", fps540));
            Toast.makeText(this, "This phone may be too slow for smooth video. Consider using a newer device.", Toast.LENGTH_LONG).show();
        } else {
            textDetection.setText(String.format("Auto-detected: 540p (%.1f FPS)", fps540));
        }
    }

    private synchronized void startStreaming() {
        if (isStreaming) return;

        encoder = new TcpServerEncoder(actualWidth, actualHeight, actualBitrate);
        encoder.setStatusListener((status, connected) -> {
            runOnUiThread(() -> {
                textStatus.setText("Status: " + status);
                if (connected) {
                    textStatus.setTextColor(Color.GREEN);
                } else {
                    textStatus.setTextColor(Color.YELLOW);
                }
                if (streamingService != null) {
                    streamingService.updateNotification(status);
                }
            });
        });

        encoder.start();
        rvmncnn.setEncoderWindow(encoder.getInputSurface());

        Intent intent = new Intent(this, StreamingService.class);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent);
        } else {
            startService(intent);
        }
        bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE);

        isStreaming = true;
        buttonStartStop.setText("STOP STREAM");
        buttonStartStop.setBackgroundTintList(android.content.res.ColorStateList.valueOf(Color.RED));
    }

    private synchronized void stopStreaming() {
        if (!isStreaming) return;

        rvmncnn.setEncoderWindow(null);

        if (encoder != null) {
            encoder.stop();
            encoder = null;
        }

        if (isServiceBound) {
            unbindService(serviceConnection);
            isServiceBound = false;
        }

        Intent intent = new Intent(this, StreamingService.class);
        stopService(intent);

        isStreaming = false;
        buttonStartStop.setText("START STREAM");
        buttonStartStop.setBackgroundTintList(android.content.res.ColorStateList.valueOf(Color.parseColor("#4CAF50")));
        textStatus.setText("Status: Not connected");
        textStatus.setTextColor(Color.YELLOW);
    }

    private void startFpsMonitor() {
        fpsRunnable = new Runnable() {
            @Override
            public void run() {
                float currentFps = rvmncnn.getCurrentFps();
                textFps.setText(String.format("FPS: %.1f", currentFps));
                fpsHandler.postDelayed(this, 1000);
            }
        };
        fpsHandler.postDelayed(fpsRunnable, 1000);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        rvmncnn.setOutputWindow(holder.getSurface());
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        rvmncnn.setOutputWindow(null);
    }

    @Override
    public void onResume() {
        super.onResume();
        boolean hasCameraPerm = ContextCompat.checkSelfPermission(getApplicationContext(), Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED;
        rvmncnn.setCameraPermission(hasCameraPerm);
        rvmncnn.setCameraActive(true);
    }

    @Override
    public void onPause() {
        // Keep camera running if actively streaming in background!
        if (!isStreaming) {
            rvmncnn.setCameraActive(false);
        }
        super.onPause();
    }

    @Override
    public void onDestroy() {
        fpsHandler.removeCallbacks(fpsRunnable);
        stopStreaming();
        super.onDestroy();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_PERMISSIONS) {
            boolean granted = ContextCompat.checkSelfPermission(getApplicationContext(), Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED;
            rvmncnn.setCameraPermission(granted);
        }
    }
}
