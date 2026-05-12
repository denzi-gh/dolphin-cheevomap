// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.cheevomap

import androidx.annotation.Keep

object CheevoMapModel {
    @Keep
    data class Entry(
        val id: String,
        val label: String,
        val group: String,
        val valueStr: String,
        val visible: Boolean,
        val iconPath: String?,
        val iconSlots: Array<String>?
    ) {
        override fun equals(other: Any?): Boolean {
            if (this === other) return true
            if (other !is Entry) return false
            if (id != other.id) return false
            if (label != other.label) return false
            if (group != other.group) return false
            if (valueStr != other.valueStr) return false
            if (visible != other.visible) return false
            if (iconPath != other.iconPath) return false
            if (iconSlots != null) {
                if (other.iconSlots == null || !iconSlots.contentEquals(other.iconSlots))
                    return false
            } else if (other.iconSlots != null) {
                return false
            }
            return true
        }

        override fun hashCode(): Int {
            var result = id.hashCode()
            result = 31 * result + label.hashCode()
            result = 31 * result + group.hashCode()
            result = 31 * result + valueStr.hashCode()
            result = 31 * result + visible.hashCode()
            result = 31 * result + (iconPath?.hashCode() ?: 0)
            result = 31 * result + (iconSlots?.contentHashCode() ?: 0)
            return result
        }
    }

    @Keep
    fun interface Listener {
        fun onChanged()
    }

    @JvmStatic external fun isLoaded(): Boolean
    @JvmStatic external fun getCurrentTitle(): String
    @JvmStatic external fun getEntries(): Array<Entry>
    @JvmStatic external fun registerListener(listener: Listener)
    @JvmStatic external fun unregisterListener()
}
