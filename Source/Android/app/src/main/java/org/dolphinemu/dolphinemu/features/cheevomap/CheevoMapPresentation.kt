// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.cheevomap

import android.app.Presentation
import android.content.Context
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Display
import android.widget.TextView
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import org.dolphinemu.dolphinemu.R

class CheevoMapPresentation(context: Context, display: Display)
    : Presentation(context, display) {

    private lateinit var titleView: TextView
    private lateinit var listView: RecyclerView
    private val adapter = CheevoMapAdapter()
    private val mainHandler = Handler(Looper.getMainLooper())
    private val listener = CheevoMapModel.Listener { mainHandler.post { refresh() } }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.presentation_cheevomap)
        titleView = findViewById(R.id.cheevomap_title)
        listView = findViewById(R.id.cheevomap_list)
        listView.layoutManager = LinearLayoutManager(context)
        listView.adapter = adapter
        CheevoMapModel.registerListener(listener)
        refresh()
    }

    override fun onDetachedFromWindow() {
        CheevoMapModel.unregisterListener()
        super.onDetachedFromWindow()
    }

    private fun refresh() {
        titleView.text = CheevoMapModel.getCurrentTitle()
        adapter.submitList(CheevoMapModel.getEntries().filter { it.visible })
    }
}
