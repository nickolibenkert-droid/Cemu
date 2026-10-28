package info.cemu.cemu.settings.input.controller

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import info.cemu.cemu.common.android.usb.DolphinBar
import info.cemu.cemu.common.ui.components.SingleSelection
import info.cemu.cemu.common.ui.localization.tr
import info.cemu.cemu.nativeinterface.NativeWiimote
import kotlinx.coroutines.delay

private const val STATUS_REFRESH_MS = 1000L

/**
 * Picks a real Wii Remote (through a DolphinBar in mode 4) for this emulated Wii Remote.
 * Choosing one replaces the button mappings with the remote's own buttons, IR pointer and motion
 */
@Composable
fun RealWiimoteSelection(
    realWiimote: Int,
    onRealWiimoteChanged: (Int) -> Unit,
) {
    val barState by DolphinBar.state.collectAsState()
    var connected by remember { mutableStateOf(BooleanArray(NativeWiimote.MAX_WIIMOTES)) }

    LaunchedEffect(Unit) {
        DolphinBar.refresh()
        while (true) {
            connected = NativeWiimote.getConnectedWiimotes()
            delay(STATUS_REFRESH_MS)
        }
    }

    fun wiimoteToString(wiimote: Int): String {
        if (wiimote == NativeWiimote.NO_WIIMOTE) {
            return tr("None")
        }
        val name = tr("Wii Remote {0}", wiimote + 1)
        return if (connected.getOrElse(wiimote) { false }) tr("{0} (connected)", name) else name
    }

    Column {
        SingleSelection(
            label = tr("Real Wii Remote (DolphinBar)"),
            choice = realWiimote,
            choices = listOf(NativeWiimote.NO_WIIMOTE) + (0 until NativeWiimote.MAX_WIIMOTES),
            choiceToString = ::wiimoteToString,
            onChoiceChanged = onRealWiimoteChanged,
        )

        val status = when (barState) {
            DolphinBar.State.NOT_FOUND -> tr("DolphinBar not found. Plug it in and set it to mode 4")
            DolphinBar.State.NO_PERMISSION -> tr("DolphinBar found, waiting for USB permission")
            DolphinBar.State.OPEN_FAILED -> tr("DolphinBar could not be opened. Close other apps using it, then unplug it and reconnect it")
            DolphinBar.State.OPEN -> tr("DolphinBar ready. Press 1+2 on a Wii Remote if it is not connected")
        }
        Text(text = status, modifier = Modifier.padding(horizontal = 8.dp))

        if (barState == DolphinBar.State.NO_PERMISSION) {
            TextButton(onClick = DolphinBar::retry) {
                Text(tr("Ask for USB permission again"))
            }
        }
        if (barState == DolphinBar.State.OPEN_FAILED) {
            TextButton(onClick = DolphinBar::refresh) {
                Text(tr("Try opening DolphinBar again"))
            }
        }
    }
}
