package com.example.vcamcontroller

import android.app.Service
import android.content.Intent
import android.graphics.PixelFormat
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.os.IBinder
import android.view.*
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.Toast
import java.io.FileDescriptor
import java.io.InputStream
import android.os.ParcelFileDescriptor
import android.view.SurfaceView
import android.view.SurfaceHolder
import kotlinx.coroutines.*
import java.nio.ByteBuffer

class OverlayService : Service(), SurfaceHolder.Callback {

    private lateinit var windowManager: WindowManager
    private lateinit var overlayView: View
    private lateinit var surfaceView: SurfaceView
    private var socket: LocalSocket? = null
    private var job: Job? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        windowManager = getSystemService(WINDOW_SERVICE) as WindowManager
        
        // Setup Floating Window Layout
        val params = WindowManager.LayoutParams(
            400, 600, // Small Preview Size
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        )
        params.gravity = Gravity.TOP or Gravity.START
        params.x = 0
        params.y = 100

        // Simple Layout construction programmatically or inflate
        val layout = FrameLayout(this)
        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        layout.addView(surfaceView, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        
        // Close Button (Top Right)
        val closeBtn = ImageView(this)
        closeBtn.setBackgroundColor(0xFFFF0000.toInt()) // Red box
        val closeParams = FrameLayout.LayoutParams(50, 50)
        closeParams.gravity = Gravity.TOP or Gravity.END
        layout.addView(closeBtn, closeParams)
        closeBtn.setOnClickListener { stopSelf() }

        overlayView = layout
        windowManager.addView(overlayView, params)
        
        // Touch Listener for Dragging (Simplified)
        overlayView.setOnTouchListener(object : View.OnTouchListener {
            var initialX = 0
            var initialY = 0
            var initialTouchX = 0f
            var initialTouchY = 0f

            override fun onTouch(v: View, event: MotionEvent): Boolean {
                when (event.action) {
                    MotionEvent.ACTION_DOWN -> {
                        initialX = params.x
                        initialY = params.y
                        initialTouchX = event.rawX
                        initialTouchY = event.rawY
                        return true
                    }
                    MotionEvent.ACTION_MOVE -> {
                        params.x = initialX + (event.rawX - initialTouchX).toInt()
                        params.y = initialY + (event.rawY - initialTouchY).toInt()
                        windowManager.updateViewLayout(overlayView, params)
                        return true
                    }
                }
                return false
            }
        })
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        startPreviewThread(holder)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
    override fun surfaceDestroyed(holder: SurfaceHolder) {
        stopPreview()
    }

    private fun startPreviewThread(holder: SurfaceHolder) {
        job = CoroutineScope(Dispatchers.IO).launch {
            try {
                // Connect to Daemon
                socket = LocalSocket()
                socket?.connect(LocalSocketAddress("/dev/socket/vcam_ipc", LocalSocketAddress.Namespace.FILESYSTEM))
                
                // Receive File Descriptor (Ashmem)
                // Java LocalSocket supports recvMsg with Ancilary FDs?
                // Actually, standard LocalSocket input stream just reads bytes.
                // To read FDs, we need `socket.ancillaryFileDescriptors`.
                // But we need to Read a byte first to trigger the message.
                
                val inputStream = socket!!.inputStream
                
                // Read control byte (daemon sends 0x00)
                val ctrlBuf = ByteArray(1)
                val readBytes = inputStream.read(ctrlBuf)
                
                // Check for FDs
                val fds = socket!!.ancillaryFileDescriptors
                if (readBytes > 0 && fds != null && fds.isNotEmpty()) {
                    val fd = fds[0]
                    val pfd = ParcelFileDescriptor.adoptFd(fd)
                    val fileStream = java.io.FileInputStream(pfd.fileDescriptor)
                    val channel = fileStream.channel
                    
                    // Map the shared memory
                    // Size = 4096 (header) + (1920*1080*1.5 * 3) ~ 9MB + 4KB
                    val totalSize = 4096 + (1920 * 1080 * 3 / 2 * 3)
                    val mapBuffer = channel.map(java.nio.channels.FileChannel.MapMode.READ_ONLY, 0, totalSize.toLong())
                    
                    val frameWidth = 1920
                    val frameHeight = 1080
                    val yuvSize = (frameWidth * frameHeight * 3) / 2
                    
                    // Bitmap for rendering (ARGB_8888)
                    // Converting YUV to RGB in Kotlin is slow, but for preview it might be okay?
                    // Or we can use ScriptIntrinsicYuvToRgb (RenderScript) or just grayscale for speed?
                    // Let's try Grayscale (Y-plane only) for v1 performance, or a simple conversion.
                    // Actually, for a "Premium" feel, it should be color. 
                    // But doing NV21 -> RGB in Kotlin loop for 1080p will lag.
                    // We'll map the Y-plane (Gray) for instant feedback first, or sub-sample.
                    
                    // Creating an int array for pixels
                    val pixels = IntArray(frameWidth * frameHeight)
                    val bitmap = android.graphics.Bitmap.createBitmap(frameWidth, frameHeight, android.graphics.Bitmap.Config.ARGB_8888)
                    
                    while (isActive) {
                        try {
                            // Read Header
                            // struct RingHeader is at offset 0
                            // volatile uint32_t write_index; // offset 0
                            // ...
                            
                            // mapBuffer.getInt(0) might not be volatile sync, but good enough loop
                            val writeIndex = mapBuffer.getInt(0)
                            
                            // Validate index
                            if (writeIndex >= 0 && writeIndex < 3) {
                                val frameOffset = 4096 + (writeIndex * yuvSize)
                                
                                // Read Y plane (first w*h bytes)
                                mapBuffer.position(frameOffset)
                                val yPlane = ByteArray(frameWidth * frameHeight)
                                mapBuffer.get(yPlane)
                                
                                // Simple NV21 to Grayscale Bitmap conversion (Copy Y to RGB)
                                // Still slow in loop.
                                // OPTIMIZATION: Only render every Nth frame or sub-sample.
                                
                                // Let's just update UI with "Frame Received: $writeIndex" for debug if we can't draw fast enough.
                                // But user wants "Live Preview".
                                // We will use a naive loop for now.
                                
                                for (i in 0 until (frameWidth * frameHeight) step 4) { // Sub-sample or full?
                                   // Full loop is too slow in Kotlin.
                                   // We really need RenderScript or OpenGL.
                                   // Given constraint, I will stick to a placeholder "Signal Active"
                                   // and try to draw a small patch.
                                }
                                
                                // BETTER: Use `YuvImage` class!
                                val yuvData = ByteArray(yuvSize)
                                mapBuffer.position(frameOffset)
                                mapBuffer.get(yuvData)
                                
                                val yuvImage = android.graphics.YuvImage(yuvData, android.graphics.ImageFormat.NV21, frameWidth, frameHeight, null)
                                val outStream = java.io.ByteArrayOutputStream()
                                // Compress to JPEG is also slow but built-in
                                yuvImage.compressToJpeg(android.graphics.Rect(0, 0, frameWidth, frameHeight), 50, outStream)
                                val jpegBytes = outStream.toByteArray()
                                val decodedBmp = android.graphics.BitmapFactory.decodeByteArray(jpegBytes, 0, jpegBytes.size)
                                
                                // Draw to Surface
                                val canvas = holder.lockCanvas()
                                if (canvas != null) {
                                    // Scale to fit
                                    val destRect = android.graphics.Rect(0, 0, canvas.width, canvas.height)
                                    canvas.drawBitmap(decodedBmp, null, destRect, null)
                                    holder.unlockCanvasAndPost(canvas)
                                }
                            }
                            
                            delay(33) // ~30fps cap
                        } catch (e: Exception) {
                            e.printStackTrace()
                            break
                        }
                    }
                    
                    fileStream.close()
                    pfd.close()
                } else {
                     withContext(Dispatchers.Main) {
                        Toast.makeText(applicationContext, "Failed to get FD from daemon", Toast.LENGTH_LONG).show()
                    }
                }
                
            } catch (e: Exception) {
                e.printStackTrace()
                withContext(Dispatchers.Main) {
                    Toast.makeText(applicationContext, "Connection Failed: ${e.message}", Toast.LENGTH_LONG).show()
                }
            }
        }
    }

    private fun stopPreview() {
        job?.cancel()
        socket?.close()
    }

    override fun onDestroy() {
        super.onDestroy()
        if (::overlayView.isInitialized) {
            windowManager.removeView(overlayView)
        }
        stopPreview()
    }
}
