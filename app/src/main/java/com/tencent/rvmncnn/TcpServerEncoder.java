package com.tencent.rvmncnn;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Build;
import android.util.Log;
import android.view.Surface;

import java.io.DataOutputStream;
import java.io.IOException;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicBoolean;

public class TcpServerEncoder {
    private static final String TAG = "FlipCam_Encoder";
    private static final int PORT = 9000;

    private int width;
    private int height;
    private int bitrate;
    private int frameRate = 30;

    private MediaCodec mediaCodec;
    private Surface inputSurface;

    private ServerSocket serverSocket;
    private Socket clientSocket;
    private DataOutputStream outputStream;

    private Thread encoderThread;
    private Thread serverThread;

    private final AtomicBoolean isRunning = new AtomicBoolean(false);
    private final AtomicBoolean isConnected = new AtomicBoolean(false);

    private byte[] spsPpsHeader = null;

    public interface StatusListener {
        void onStatusChanged(String status, boolean connected);
    }

    private StatusListener statusListener;

    public TcpServerEncoder(int width, int height, int bitrate) {
        this.width = width;
        this.height = height;
        this.bitrate = bitrate;
    }

    public void setStatusListener(StatusListener listener) {
        this.statusListener = listener;
    }

    public Surface getInputSurface() {
        return inputSurface;
    }

    public boolean isConnected() {
        return isConnected.get();
    }

    public synchronized void start() {
        if (isRunning.get()) return;
        isRunning.set(true);

        try {
            initMediaCodec();
            startTcpServer();
            startEncoderLoop();
            notifyStatus("Waiting for PC...", false);
        } catch (Exception e) {
            Log.e(TAG, "Failed to start TcpServerEncoder", e);
            stop();
        }
    }

    private void initMediaCodec() throws IOException {
        MediaFormat format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height);
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        format.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
        format.setInteger(MediaFormat.KEY_FRAME_RATE, frameRate);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
        format.setInteger(MediaFormat.KEY_PRIORITY, 0);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
        }

        mediaCodec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
        mediaCodec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        inputSurface = mediaCodec.createInputSurface();
        mediaCodec.start();
    }

    private void startTcpServer() {
        serverThread = new Thread(() -> {
            try {
                // Bind ONLY to 127.0.0.1 (loopback) for security
                serverSocket = new ServerSocket(PORT, 1, InetAddress.getByName("127.0.0.1"));
                Log.i(TAG, "TCP Server listening on 127.0.0.1:" + PORT);

                while (isRunning.get()) {
                    try {
                        Socket socket = serverSocket.accept();
                        socket.setTcpNoDelay(true);
                        synchronized (TcpServerEncoder.this) {
                            if (clientSocket != null) {
                                try { clientSocket.close(); } catch (Exception ignored) {}
                            }
                            clientSocket = socket;
                            outputStream = new DataOutputStream(socket.getOutputStream());
                            isConnected.set(true);
                        }
                        notifyStatus("Streaming to PC", true);
                        Log.i(TAG, "PC connected to TCP server!");

                        while (isRunning.get() && socket.isConnected() && !socket.isClosed()) {
                            try {
                                Thread.sleep(500);
                            } catch (InterruptedException e) {
                                break;
                            }
                        }
                    } catch (IOException e) {
                        if (isRunning.get()) {
                            Log.w(TAG, "Client socket error/disconnected");
                        }
                    } finally {
                        synchronized (TcpServerEncoder.this) {
                            isConnected.set(false);
                            outputStream = null;
                            if (clientSocket != null) {
                                try { clientSocket.close(); } catch (Exception ignored) {}
                                clientSocket = null;
                            }
                        }
                        if (isRunning.get()) {
                            notifyStatus("Waiting for PC...", false);
                        }
                    }
                }
            } catch (Exception e) {
                if (isRunning.get()) {
                    Log.e(TAG, "TCP Server exception", e);
                }
            }
        }, "FlipCam-TcpServer");
        serverThread.start();
    }

    private void startEncoderLoop() {
        encoderThread = new Thread(() -> {
            MediaCodec.BufferInfo bufferInfo = new MediaCodec.BufferInfo();
            while (isRunning.get()) {
                try {
                    int outputBufferIndex = mediaCodec.dequeueOutputBuffer(bufferInfo, 10000);
                    if (outputBufferIndex >= 0) {
                        ByteBuffer outputBuffer = mediaCodec.getOutputBuffer(outputBufferIndex);
                        if (outputBuffer != null && bufferInfo.size > 0) {
                            outputBuffer.position(bufferInfo.offset);
                            outputBuffer.limit(bufferInfo.offset + bufferInfo.size);

                            byte[] outData = new byte[bufferInfo.size];
                            outputBuffer.get(outData);

                            boolean isConfig = (bufferInfo.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0;
                            boolean isKeyFrame = (bufferInfo.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;

                            if (isConfig) {
                                spsPpsHeader = outData;
                            } else {
                                sendFrameData(outData, isKeyFrame);
                            }
                        }
                        mediaCodec.releaseOutputBuffer(outputBufferIndex, false);
                    }
                } catch (Exception e) {
                    if (isRunning.get()) {
                        Log.e(TAG, "Encoder dequeue error", e);
                    }
                }
            }
        }, "FlipCam-EncoderLoop");
        encoderThread.start();
    }

    private synchronized void sendFrameData(byte[] data, boolean isKeyFrame) {
        if (!isConnected.get() || outputStream == null) return;

        try {
            if (isKeyFrame && spsPpsHeader != null) {
                outputStream.writeInt(spsPpsHeader.length);
                outputStream.write(spsPpsHeader);
            }

            outputStream.writeInt(data.length);
            outputStream.write(data);
            outputStream.flush();
        } catch (IOException e) {
            Log.w(TAG, "Error writing frame to TCP socket, client disconnected");
            isConnected.set(false);
            if (clientSocket != null) {
                try { clientSocket.close(); } catch (Exception ignored) {}
                clientSocket = null;
            }
            outputStream = null;
            notifyStatus("Waiting for PC...", false);
        }
    }

    private void notifyStatus(String status, boolean connected) {
        if (statusListener != null) {
            statusListener.onStatusChanged(status, connected);
        }
    }

    public synchronized void stop() {
        if (!isRunning.get()) return;
        isRunning.set(false);

        if (serverSocket != null) {
            try { serverSocket.close(); } catch (Exception ignored) {}
            serverSocket = null;
        }

        if (clientSocket != null) {
            try { clientSocket.close(); } catch (Exception ignored) {}
            clientSocket = null;
        }

        if (mediaCodec != null) {
            try {
                mediaCodec.stop();
                mediaCodec.release();
            } catch (Exception ignored) {}
            mediaCodec = null;
        }

        if (inputSurface != null) {
            inputSurface.release();
            inputSurface = null;
        }

        if (serverThread != null) {
            serverThread.interrupt();
            serverThread = null;
        }

        if (encoderThread != null) {
            encoderThread.interrupt();
            encoderThread = null;
        }

        notifyStatus("Not connected", false);
    }
}
