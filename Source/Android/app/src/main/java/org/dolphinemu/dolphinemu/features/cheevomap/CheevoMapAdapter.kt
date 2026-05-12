// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.cheevomap

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import coil.load
import org.dolphinemu.dolphinemu.R
import java.io.File

class CheevoMapAdapter : ListAdapter<CheevoMapModel.Entry, CheevoMapAdapter.Holder>(DIFF) {

    class Holder(view: View) : RecyclerView.ViewHolder(view) {
        val icon: ImageView = view.findViewById(R.id.cheevomap_icon)
        val slots: LinearLayout = view.findViewById(R.id.cheevomap_slots)
        val label: TextView = view.findViewById(R.id.cheevomap_label)
        val value: TextView = view.findViewById(R.id.cheevomap_value)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Holder {
        val v = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_cheevomap_entry, parent, false)
        return Holder(v)
    }

    override fun onBindViewHolder(holder: Holder, position: Int) {
        val item = getItem(position)
        holder.label.text = item.label
        holder.value.text = item.valueStr

        // Single icon
        if (!item.iconPath.isNullOrEmpty()) {
            holder.icon.visibility = View.VISIBLE
            holder.icon.load(File(item.iconPath))
        } else {
            holder.icon.visibility = View.GONE
            holder.icon.setImageDrawable(null)
        }

        // Icon slots (fill_n / bitmap_array)
        val slots = item.iconSlots
        if (slots != null && slots.isNotEmpty()) {
            holder.slots.visibility = View.VISIBLE
            // Resize the slot row
            while (holder.slots.childCount > slots.size) {
                holder.slots.removeViewAt(holder.slots.childCount - 1)
            }
            while (holder.slots.childCount < slots.size) {
                val iv = ImageView(holder.itemView.context)
                val size = (24 * holder.itemView.context.resources.displayMetrics.density).toInt()
                val lp = LinearLayout.LayoutParams(size, size)
                lp.marginEnd = (2 * holder.itemView.context.resources.displayMetrics.density).toInt()
                iv.layoutParams = lp
                holder.slots.addView(iv)
            }
            for (i in slots.indices) {
                (holder.slots.getChildAt(i) as ImageView).load(File(slots[i]))
            }
        } else {
            holder.slots.visibility = View.GONE
            holder.slots.removeAllViews()
        }
    }

    companion object {
        val DIFF = object : DiffUtil.ItemCallback<CheevoMapModel.Entry>() {
            override fun areItemsTheSame(a: CheevoMapModel.Entry, b: CheevoMapModel.Entry) =
                a.id == b.id
            override fun areContentsTheSame(a: CheevoMapModel.Entry, b: CheevoMapModel.Entry) =
                a == b
        }
    }
}
