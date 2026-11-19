package com.typex2.recomp

import android.os.Build
import android.os.Bundle
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.webkit.WebView
import androidx.activity.OnBackPressedCallback
import androidx.activity.enableEdgeToEdge
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

class MainActivity : TauriActivity() {
  // Back (a controller's B too) goes to the page first: it closes a menu or returns to the
  // library (window.x2Back, useControllerNav); only when it has nothing to do does the app leave.
  override val handleBackNavigation: Boolean = false

  // Controller buttons and triggers, passed to the page as X2_PAD names (window.x2PadButton): the
  // WebView's Gamepad API shows some pads only after certain presses, and an arcade stick's L2 / R2
  // only as axes; the layout editor's Assign reads these (lib/gamepad.ts). Nothing is consumed.
  private var page: WebView? = null
  private val triggerDown = BooleanArray(2)

  private fun padButton(name: String, down: Boolean) {
    page?.post { page?.evaluateJavascript("window.x2PadButton && window.x2PadButton('$name', $down)", null) }
  }

  override fun dispatchKeyEvent(event: KeyEvent): Boolean {
    val name = when (event.keyCode) {
      KeyEvent.KEYCODE_BUTTON_A -> "south"
      KeyEvent.KEYCODE_BUTTON_B -> "east"
      KeyEvent.KEYCODE_BUTTON_X -> "west"
      KeyEvent.KEYCODE_BUTTON_Y -> "north"
      KeyEvent.KEYCODE_BUTTON_L1 -> "l1"
      KeyEvent.KEYCODE_BUTTON_R1 -> "r1"
      KeyEvent.KEYCODE_BUTTON_L2 -> "l2"
      KeyEvent.KEYCODE_BUTTON_R2 -> "r2"
      KeyEvent.KEYCODE_BUTTON_THUMBL -> "l3"
      KeyEvent.KEYCODE_BUTTON_THUMBR -> "r3"
      else -> null
    }
    if (name != null && event.repeatCount == 0 && event.action != KeyEvent.ACTION_MULTIPLE)
      padButton(name, event.action == KeyEvent.ACTION_DOWN)
    return super.dispatchKeyEvent(event)
  }

  override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
    if (event.isFromSource(InputDevice.SOURCE_JOYSTICK) && event.action == MotionEvent.ACTION_MOVE) {
      val l = maxOf(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE))
      val r = maxOf(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS))
      for ((i, v) in floatArrayOf(l, r).withIndex()) {
        val down = v > 0.5f
        if (down != triggerDown[i]) {
          triggerDown[i] = down
          padButton(if (i == 0) "l2" else "r2", down)
        }
      }
    }
    return super.dispatchGenericMotionEvent(event)
  }

  override fun onWebViewCreate(webView: WebView) {
    page = webView
    // draws (a controller's moves and scrolls included, not only touch) at the fastest rate
    if (Build.VERSION.SDK_INT >= 35) webView.setRequestedFrameRate(View.REQUESTED_FRAME_RATE_CATEGORY_HIGH)
    val callback = object : OnBackPressedCallback(true) {
      override fun handleOnBackPressed() {
        webView.evaluateJavascript("window.x2Back ? window.x2Back() : false") { handled ->
          if (handled != "true") {
            isEnabled = false
            onBackPressedDispatcher.onBackPressed()
            isEnabled = true
          }
        }
      }
    }
    onBackPressedDispatcher.addCallback(this, callback)
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    enableEdgeToEdge()
    super.onCreate(savedInstanceState)
    // Only a display cutout needs room (the webview gets no usable safe-area insets); the
    // status and navigation bars are hidden (immersive, as the games are).
    val content = findViewById<View>(android.R.id.content)
    ViewCompat.setOnApplyWindowInsetsListener(content) { v, insets ->
      val cut = insets.getInsets(WindowInsetsCompat.Type.displayCutout())
      v.setPadding(cut.left, cut.top, cut.right, cut.bottom)
      WindowInsetsCompat.CONSUMED
    }
    hideSystemBars()
    preferFastestRefresh()
  }

  // The launcher at the display's fastest refresh rate (90 Hz on the test tablet, which an
  // adaptive setting otherwise holds at 60 for the webview): smoother scrolling. The games run
  // in their own activity, at the 60 Hz they are made for.
  private fun preferFastestRefresh() {
    val d = display ?: return
    val cur = d.mode
    val best = d.supportedModes
      .filter { it.physicalWidth == cur.physicalWidth && it.physicalHeight == cur.physicalHeight }
      .maxByOrNull { it.refreshRate } ?: return
    if (best.modeId != cur.modeId) window.attributes = window.attributes.apply { preferredDisplayModeId = best.modeId }
  }

  override fun onWindowFocusChanged(hasFocus: Boolean) {
    super.onWindowFocusChanged(hasFocus)
    if (hasFocus) hideSystemBars() // again after a dialog, the permission screen or a game
  }

  private fun hideSystemBars() {
    WindowCompat.getInsetsController(window, window.decorView).apply {
      systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
      hide(WindowInsetsCompat.Type.systemBars())
    }
  }
}
