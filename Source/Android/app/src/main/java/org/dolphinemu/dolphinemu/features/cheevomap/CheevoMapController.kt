// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.cheevomap

import android.content.Context
import android.hardware.display.DisplayManager
import android.view.Display
import android.view.ViewGroup
import org.dolphinemu.dolphinemu.features.settings.model.BooleanSetting

/**
 * Decides whether to render the CheevoMap on the device's secondary presentation
 * display (e.g. AYN Thor's bottom screen) or as an in-emulation overlay on the main
 * display. Reattaches when displays are hot-plugged. The caller drives lifecycle
 * via [attach] / [detach] from EmulationFragment.onResume/onPause.
 */
class CheevoMapController(private val context: Context) {

    private val displayManager =
        context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager

    private var presentation: CheevoMapPresentation? = null
    private var overlay: CheevoMapOverlayView? = null
    private var fallbackParent: ViewGroup? = null

    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) = rebind()
        override fun onDisplayChanged(displayId: Int) = rebind()
        override fun onDisplayRemoved(displayId: Int) = rebind()
    }

    fun attach(fallbackParent: ViewGroup) {
        if (!BooleanSetting.MAIN_SHOW_CHEEVOMAP.boolean) return
        this.fallbackParent = fallbackParent
        displayManager.registerDisplayListener(displayListener, null)
        bindToBestDisplay()
    }

    fun detach() {
        runCatching { displayManager.unregisterDisplayListener(displayListener) }
        presentation?.dismiss()
        presentation = null
        removeOverlay()
        fallbackParent = null
    }

    private fun rebind() {
        if (fallbackParent == null) return
        bindToBestDisplay()
    }

    private fun bindToBestDisplay() {
        val parent = fallbackParent ?: return
        val secondary = pickSecondaryDisplay()
        val useSecondary = BooleanSetting.MAIN_CHEEVOMAP_USE_SECONDARY_DISPLAY.boolean
        if (useSecondary && secondary != null) {
            removeOverlay()
            val current = presentation
            if (current == null || current.display.displayId != secondary.displayId) {
                current?.dismiss()
                presentation = CheevoMapPresentation(context, secondary).also { it.show() }
            }
        } else {
            presentation?.dismiss()
            presentation = null
            if (overlay == null) {
                overlay = CheevoMapOverlayView(context).also { parent.addView(it) }
            }
        }
    }

    private fun pickSecondaryDisplay(): Display? {
        val presentationDisplays =
            displayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)
        if (presentationDisplays.isNotEmpty()) return presentationDisplays.first()
        // Fallback: any display that isn't the default one
        return displayManager.displays.firstOrNull { it.displayId != Display.DEFAULT_DISPLAY }
    }

    private fun removeOverlay() {
        overlay?.let { (it.parent as? ViewGroup)?.removeView(it) }
        overlay = null
    }
}
