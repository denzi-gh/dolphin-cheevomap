// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.cheevomap

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.LayoutInflater
import android.widget.FrameLayout
import android.widget.TextView
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import org.dolphinemu.dolphinemu.R

class CheevoMapOverlayView(context: Context) : FrameLayout(context) {
    private val titleView: TextView
    private val listView: RecyclerView
    private val adapter = CheevoMapAdapter()
    private val mainHandler = Handler(Looper.getMainLooper())
    private val listener = CheevoMapModel.Listener { mainHandler.post { refresh() } }

    init {
        LayoutInflater.from(context).inflate(R.layout.overlay_cheevomap, this, true)
        titleView = findViewById(R.id.cheevomap_title)
        listView = findViewById(R.id.cheevomap_list)
        listView.layoutManager = LinearLayoutManager(context)
        listView.adapter = adapter
        // Default placement when added to a parent FrameLayout
        layoutParams = LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT,
            Gravity.TOP or Gravity.END)
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        CheevoMapModel.registerListener(listener)
        refresh()
    }

    override fun onDetachedFromWindow() {
        CheevoMapModel.unregisterListener()
        super.onDetachedFromWindow()
    }

    private fun refresh() {
        val title = CheevoMapModel.getCurrentTitle()
        titleView.text = title
        val entries = CheevoMapModel.getEntries().filter { it.visible }
        visibility = if (CheevoMapModel.isLoaded() && entries.isNotEmpty()) VISIBLE else GONE
        adapter.submitList(entries)
    }
}
