package com.typex2.recomp

import android.os.Bundle
import android.view.View
import androidx.activity.enableEdgeToEdge
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

class MainActivity : TauriActivity() {
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
