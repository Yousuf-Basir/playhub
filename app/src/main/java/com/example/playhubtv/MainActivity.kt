package com.example.playhubtv

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.View
import android.view.Window
import android.window.OnBackInvokedCallback
import android.window.OnBackInvokedDispatcher
import android.widget.ImageView
import android.widget.SeekBar
import android.widget.TextView
import com.google.zxing.BarcodeFormat
import com.google.zxing.EncodeHintType
import com.google.zxing.qrcode.QRCodeWriter
import fi.iki.elonen.NanoHTTPD
import org.videolan.libvlc.LibVLC
import org.videolan.libvlc.Media
import org.videolan.libvlc.MediaPlayer
import org.videolan.libvlc.util.VLCVideoLayout
import java.net.Inet4Address
import java.net.NetworkInterface
import java.net.URLDecoder
import java.nio.charset.StandardCharsets
import java.util.EnumMap

class MainActivity : Activity() {
    private lateinit var guidePanel: View
    private lateinit var qrCode: ImageView
    private lateinit var statusText: TextView
    private lateinit var urlText: TextView
    private lateinit var videoLayout: VLCVideoLayout
    private lateinit var playerControls: View
    private lateinit var playerTitle: TextView
    private lateinit var playerStatus: TextView
    private lateinit var playPauseButton: TextView
    private lateinit var playerSeekbar: SeekBar
    private lateinit var currentTime: TextView
    private lateinit var durationTime: TextView

    private var controlServer: StreamControlServer? = null
    private var libVlc: LibVLC? = null
    private var mediaPlayer: MediaPlayer? = null
    private var controlUrl: String? = null
    private var currentStreamUrl: String? = null
    private var backCallback: OnBackInvokedCallback? = null
    private val uiHandler = Handler(Looper.getMainLooper())
    private val progressUpdater = object : Runnable {
        override fun run() {
            updatePlaybackProgress()
            uiHandler.postDelayed(this, PROGRESS_UPDATE_MS)
        }
    }
    private val hideControlsRunnable = Runnable {
        if (mediaPlayer?.isPlaying == true) {
            playerControls.visibility = View.GONE
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestWindowFeature(Window.FEATURE_NO_TITLE)
        setContentView(R.layout.activity_main)

        guidePanel = findViewById(R.id.guide_panel)
        qrCode = findViewById(R.id.qr_code)
        statusText = findViewById(R.id.status_text)
        urlText = findViewById(R.id.url_text)
        videoLayout = findViewById(R.id.video_layout)
        playerControls = findViewById(R.id.player_controls)
        playerTitle = findViewById(R.id.player_title)
        playerStatus = findViewById(R.id.player_status)
        playPauseButton = findViewById(R.id.play_pause_button)
        playerSeekbar = findViewById(R.id.player_seekbar)
        currentTime = findViewById(R.id.current_time)
        durationTime = findViewById(R.id.duration_time)

        playPauseButton.setOnClickListener {
            togglePlayPause()
            showControlsTemporarily()
        }
        playerControls.setOnClickListener {
            togglePlayPause()
            showControlsTemporarily()
        }

        startLocalControlServer()
        preparePlayer()
        registerBackHandler()
    }

    override fun onDestroy() {
        super.onDestroy()
        uiHandler.removeCallbacksAndMessages(null)
        unregisterBackHandler()
        controlServer?.stop()
        mediaPlayer?.stop()
        mediaPlayer?.detachViews()
        mediaPlayer?.release()
        libVlc?.release()
    }

    override fun onBackPressed() {
        handleBackAction()
    }

    private fun handleBackAction() {
        if (videoLayout.visibility == View.VISIBLE && playerControls.visibility == View.VISIBLE) {
            playerControls.visibility = View.GONE
        } else if (videoLayout.visibility == View.VISIBLE) {
            showControlsTemporarily()
        } else {
            moveTaskToBack(true)
        }
    }

    private fun registerBackHandler() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return

        val callback = OnBackInvokedCallback {
            handleBackAction()
        }
        backCallback = callback
        onBackInvokedDispatcher.registerOnBackInvokedCallback(
            OnBackInvokedDispatcher.PRIORITY_OVERLAY,
            callback
        )
    }

    private fun unregisterBackHandler() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return

        backCallback?.let {
            onBackInvokedDispatcher.unregisterOnBackInvokedCallback(it)
        }
        backCallback = null
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (videoLayout.visibility != View.VISIBLE) return super.dispatchKeyEvent(event)

        val playerKey = when (event.keyCode) {
            KeyEvent.KEYCODE_BACK,
            KeyEvent.KEYCODE_ESCAPE,
            KeyEvent.KEYCODE_DPAD_CENTER,
            KeyEvent.KEYCODE_ENTER,
            KeyEvent.KEYCODE_SPACE,
            KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE,
            KeyEvent.KEYCODE_MEDIA_PLAY,
            KeyEvent.KEYCODE_MEDIA_PAUSE,
            KeyEvent.KEYCODE_DPAD_LEFT,
            KeyEvent.KEYCODE_MEDIA_REWIND,
            KeyEvent.KEYCODE_DPAD_RIGHT,
            KeyEvent.KEYCODE_MEDIA_FAST_FORWARD,
            KeyEvent.KEYCODE_DPAD_UP,
            KeyEvent.KEYCODE_DPAD_DOWN,
            KeyEvent.KEYCODE_MENU -> true

            else -> false
        }

        if (!playerKey) return super.dispatchKeyEvent(event)
        if (event.action != KeyEvent.ACTION_DOWN || event.repeatCount > 0) {
            return true
        }

        return when (event.keyCode) {
            KeyEvent.KEYCODE_BACK,
            KeyEvent.KEYCODE_ESCAPE -> {
                if (playerControls.visibility == View.VISIBLE) {
                    playerControls.visibility = View.GONE
                } else {
                    showControlsTemporarily()
                }
                true
            }

            KeyEvent.KEYCODE_DPAD_CENTER,
            KeyEvent.KEYCODE_ENTER,
            KeyEvent.KEYCODE_SPACE,
            KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE -> {
                togglePlayPause()
                showControlsTemporarily()
                true
            }

            KeyEvent.KEYCODE_MEDIA_PLAY -> {
                mediaPlayer?.play()
                showControlsTemporarily()
                true
            }

            KeyEvent.KEYCODE_MEDIA_PAUSE -> {
                mediaPlayer?.pause()
                showControls()
                true
            }

            KeyEvent.KEYCODE_DPAD_LEFT,
            KeyEvent.KEYCODE_MEDIA_REWIND -> {
                seekBy(-SEEK_STEP_MS)
                showControlsTemporarily()
                true
            }

            KeyEvent.KEYCODE_DPAD_RIGHT,
            KeyEvent.KEYCODE_MEDIA_FAST_FORWARD -> {
                seekBy(SEEK_STEP_MS)
                showControlsTemporarily()
                true
            }

            KeyEvent.KEYCODE_DPAD_UP,
            KeyEvent.KEYCODE_DPAD_DOWN,
            KeyEvent.KEYCODE_MENU -> {
                showControlsTemporarily()
                true
            }

            else -> super.dispatchKeyEvent(event)
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (videoLayout.visibility != View.VISIBLE) return super.onKeyDown(keyCode, event)

        return when (keyCode) {
            KeyEvent.KEYCODE_DPAD_CENTER,
            KeyEvent.KEYCODE_ENTER,
            KeyEvent.KEYCODE_SPACE,
            KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE -> {
                togglePlayPause()
                showControlsTemporarily()
                true
            }

            KeyEvent.KEYCODE_MEDIA_PLAY -> {
                mediaPlayer?.play()
                showControlsTemporarily()
                true
            }

            KeyEvent.KEYCODE_MEDIA_PAUSE -> {
                mediaPlayer?.pause()
                showControls()
                true
            }

            KeyEvent.KEYCODE_DPAD_LEFT,
            KeyEvent.KEYCODE_MEDIA_REWIND -> {
                seekBy(-SEEK_STEP_MS)
                showControlsTemporarily()
                true
            }

            KeyEvent.KEYCODE_DPAD_RIGHT,
            KeyEvent.KEYCODE_MEDIA_FAST_FORWARD -> {
                seekBy(SEEK_STEP_MS)
                showControlsTemporarily()
                true
            }

            else -> {
                showControlsTemporarily()
                super.onKeyDown(keyCode, event)
            }
        }
    }

    private fun startLocalControlServer() {
        val hostAddress = findLocalIpAddress()

        if (hostAddress == null) {
            statusText.text = "Connect this TV to Wi-Fi or Ethernet, then restart the app."
            return
        }

        val server = startServerOnAvailablePort { streamUrl ->
            runOnUiThread { playStream(streamUrl) }
        }

        if (server == null) {
            statusText.text = "Could not start the local control server."
            return
        }

        controlServer = server
        val deviceUrl = "http://$hostAddress:${server.controlPort}/"
        val isEmulator = hostAddress == EMULATOR_PRIVATE_IP
        val displayUrl = if (isEmulator) {
            "http://127.0.0.1:${server.controlPort}/"
        } else {
            deviceUrl
        }

        controlUrl = deviceUrl

        qrCode.setImageBitmap(createQrBitmap(displayUrl, 640))
        if (isEmulator) {
            statusText.text = "Android emulator detected. Use this from the same Windows PC after ADB port forwarding. Phone QR testing needs a real Android TV on your LAN."
            urlText.text = "Run: adb forward tcp:${server.controlPort} tcp:${server.controlPort}\nOpen: $displayUrl"
        } else {
            statusText.text = "Scan the QR code with your phone. Enter a stream URL there to play it on this TV."
            urlText.text = deviceUrl
        }
    }

    private fun preparePlayer() {
        libVlc = LibVLC(
            this,
            arrayListOf(
                "--network-caching=1500",
                "--file-caching=1500",
                "--avcodec-hw=any"
            )
        )
        mediaPlayer = MediaPlayer(libVlc).apply {
            attachViews(videoLayout, null, false, false)
            setVideoScale(MediaPlayer.ScaleType.SURFACE_BEST_FIT)
            setEventListener { event ->
                runOnUiThread { handlePlayerEvent(event) }
            }
        }
    }

    private fun playStream(streamUrl: String) {
        val vlc = libVlc ?: return
        val player = mediaPlayer ?: return

        guidePanel.visibility = View.GONE
        videoLayout.visibility = View.VISIBLE
        currentStreamUrl = streamUrl
        playerTitle.text = streamUrl.substringAfterLast('/').ifBlank { "Streaming video" }
        playerStatus.text = "Opening stream..."
        currentTime.text = getString(R.string.zero_time)
        durationTime.text = getString(R.string.unknown_duration)
        playerSeekbar.progress = 0
        showControlsTemporarily()

        player.stop()

        val media = Media(vlc, Uri.parse(streamUrl)).apply {
            setHWDecoderEnabled(true, false)
            addOption(":network-caching=1500")
            addOption(":file-caching=1500")
        }

        player.media = media
        media.release()
        player.play()
        uiHandler.removeCallbacks(progressUpdater)
        uiHandler.post(progressUpdater)
    }

    private fun handlePlayerEvent(event: MediaPlayer.Event) {
        when (event.type) {
            MediaPlayer.Event.Opening -> playerStatus.text = "Opening stream..."
            MediaPlayer.Event.Buffering -> playerStatus.text = "Buffering ${event.buffering.toInt()}%"
            MediaPlayer.Event.Playing -> {
                playerStatus.text = currentStreamUrl.orEmpty()
                playPauseButton.text = getString(R.string.pause)
                showControlsTemporarily()
            }

            MediaPlayer.Event.Paused -> {
                playerStatus.text = "Paused"
                playPauseButton.text = getString(R.string.play)
                showControls()
            }

            MediaPlayer.Event.Stopped -> {
                playPauseButton.text = getString(R.string.play)
            }

            MediaPlayer.Event.EndReached -> {
                playerStatus.text = "Stream ended"
                playPauseButton.text = getString(R.string.play)
                showControls()
            }

            MediaPlayer.Event.EncounteredError -> {
                playerStatus.text = "Could not play this stream"
                showControls()
            }
        }
    }

    private fun togglePlayPause() {
        val player = mediaPlayer ?: return
        if (player.isPlaying) {
            player.pause()
        } else {
            player.play()
        }
        updatePlaybackProgress()
    }

    private fun seekBy(deltaMs: Long) {
        val player = mediaPlayer ?: return
        val length = player.length
        if (length <= 0L || !player.isSeekable) return

        val nextTime = (player.time + deltaMs).coerceIn(0L, length)
        player.time = nextTime
        updatePlaybackProgress()
    }

    private fun updatePlaybackProgress() {
        val player = mediaPlayer ?: return
        val length = player.length
        val time = player.time.coerceAtLeast(0L)

        currentTime.text = formatDuration(time)
        durationTime.text = if (length > 0L) formatDuration(length) else getString(R.string.unknown_duration)
        playerSeekbar.progress = if (length > 0L) ((time * 1000L) / length).toInt().coerceIn(0, 1000) else 0
        playPauseButton.text = if (player.isPlaying) getString(R.string.pause) else getString(R.string.play)
    }

    private fun showControls() {
        playerControls.visibility = View.VISIBLE
        playerControls.requestFocus()
        uiHandler.removeCallbacks(hideControlsRunnable)
    }

    private fun showControlsTemporarily() {
        showControls()
        uiHandler.postDelayed(hideControlsRunnable, CONTROLS_HIDE_DELAY_MS)
    }

    private fun formatDuration(milliseconds: Long): String {
        val totalSeconds = milliseconds / 1000L
        val seconds = totalSeconds % 60L
        val minutes = (totalSeconds / 60L) % 60L
        val hours = totalSeconds / 3600L

        return if (hours > 0L) {
            "%d:%02d:%02d".format(hours, minutes, seconds)
        } else {
            "%d:%02d".format(minutes, seconds)
        }
    }

    private fun startServerOnAvailablePort(onStreamUrl: (String) -> Unit): StreamControlServer? {
        for (port in CONTROL_PORT..CONTROL_PORT + 20) {
            val server = StreamControlServer(port, onStreamUrl)
            try {
                server.start(NanoHTTPD.SOCKET_READ_TIMEOUT, false)
                return server
            } catch (_: Exception) {
                server.stop()
            }
        }
        return null
    }

    private fun createQrBitmap(content: String, size: Int): Bitmap {
        val hints = EnumMap<EncodeHintType, Any>(EncodeHintType::class.java).apply {
            put(EncodeHintType.MARGIN, 1)
        }
        val matrix = QRCodeWriter().encode(content, BarcodeFormat.QR_CODE, size, size, hints)
        val pixels = IntArray(size * size)

        for (y in 0 until size) {
            for (x in 0 until size) {
                pixels[y * size + x] = if (matrix[x, y]) Color.BLACK else Color.WHITE
            }
        }

        return Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888).apply {
            setPixels(pixels, 0, size, 0, 0, size, size)
        }
    }

    private fun findLocalIpAddress(): String? {
        val interfaces = NetworkInterface.getNetworkInterfaces().toList()
        for (networkInterface in interfaces) {
            if (!networkInterface.isUp || networkInterface.isLoopback) continue

            val address = networkInterface.inetAddresses.toList()
                .filterIsInstance<Inet4Address>()
                .firstOrNull { !it.isLoopbackAddress && !it.isLinkLocalAddress }

            if (address != null) return address.hostAddress
        }

        return null
    }

    companion object {
        private const val CONTROL_PORT = 8080
        private const val EMULATOR_PRIVATE_IP = "10.0.2.15"
        private const val CONTROLS_HIDE_DELAY_MS = 5000L
        private const val PROGRESS_UPDATE_MS = 500L
        private const val SEEK_STEP_MS = 10_000L
    }
}

private class StreamControlServer(
    private val portNumber: Int,
    private val onStreamUrl: (String) -> Unit
) : NanoHTTPD(portNumber) {
    val controlPort: Int
        get() = portNumber

    override fun serve(session: IHTTPSession): Response {
        return when {
            session.method == Method.GET && session.uri == "/" -> htmlResponse(controlPage())
            session.method == Method.POST && session.uri == "/play" -> handlePlayRequest(session)
            session.method == Method.GET && session.uri == "/play" -> handlePlayRequest(session)
            else -> newFixedLengthResponse(Response.Status.NOT_FOUND, MIME_PLAINTEXT, "Not found")
        }
    }

    private fun handlePlayRequest(session: IHTTPSession): Response {
        val body = HashMap<String, String>()
        if (session.method == Method.POST) {
            try {
                session.parseBody(body)
            } catch (error: Exception) {
                return newFixedLengthResponse(
                    Response.Status.BAD_REQUEST,
                    MIME_PLAINTEXT,
                    "Invalid request body: ${error.message}"
                )
            }
        }

        val streamUrl = session.parameters["url"]?.firstOrNull()
            ?: session.parms["url"]
            ?: parseFormField(body["postData"], "url")

        if (streamUrl.isNullOrBlank()) {
            return newFixedLengthResponse(Response.Status.BAD_REQUEST, MIME_PLAINTEXT, "Missing url")
        }

        onStreamUrl(streamUrl.trim())

        return jsonResponse("""{"ok":true}""")
    }

    private fun controlPage(): String {
        return """
            <!doctype html>
            <html lang="en">
            <head>
              <meta charset="utf-8">
              <meta name="viewport" content="width=device-width, initial-scale=1">
              <title>Play Hub TV</title>
              <style>
                :root { color-scheme: dark; font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; }
                body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: #111; color: white; }
                main { width: min(92vw, 560px); }
                h1 { font-size: 2rem; margin: 0 0 1rem; }
                label { display: block; margin-bottom: .5rem; color: #ddd; }
                input { width: 100%; box-sizing: border-box; padding: 1rem; border-radius: .5rem; border: 1px solid #555; background: #1d1d1d; color: white; font-size: 1rem; }
                button { width: 100%; margin-top: 1rem; padding: 1rem; border: 0; border-radius: .5rem; background: #2f80ed; color: white; font-size: 1rem; font-weight: 700; }
                p { min-height: 1.5rem; color: #9ad0ff; }
              </style>
            </head>
            <body>
              <main>
                <h1>Play on TV</h1>
                <form id="stream-form">
                  <label for="url">Video stream URL</label>
                  <input id="url" name="url" type="url" inputmode="url" placeholder="https://example.com/video.mkv" required autofocus>
                  <button type="submit">Start streaming</button>
                </form>
                <p id="status"></p>
              </main>
              <script>
                const form = document.getElementById('stream-form');
                const status = document.getElementById('status');
                form.addEventListener('submit', async (event) => {
                  event.preventDefault();
                  status.textContent = 'Sending...';
                  const body = new URLSearchParams(new FormData(form));
                  const response = await fetch('/play', { method: 'POST', body });
                  status.textContent = response.ok ? 'Stream sent to TV.' : 'Could not send stream.';
                });
              </script>
            </body>
            </html>
        """.trimIndent()
    }

    private fun parseFormField(postData: String?, fieldName: String): String? {
        if (postData.isNullOrBlank()) return null

        return postData.split("&")
            .mapNotNull { part ->
                val keyValue = part.split("=", limit = 2)
                if (keyValue.size != 2) return@mapNotNull null

                val key = keyValue[0].urlDecode()
                val value = keyValue[1].urlDecode()
                key to value
            }
            .firstOrNull { it.first == fieldName }
            ?.second
    }

    private fun String.urlDecode(): String =
        URLDecoder.decode(this, StandardCharsets.UTF_8.name())

    private fun htmlResponse(html: String): Response =
        newFixedLengthResponse(Response.Status.OK, "text/html; charset=utf-8", html)

    private fun jsonResponse(json: String): Response =
        newFixedLengthResponse(Response.Status.OK, "application/json; charset=utf-8", json)
}
