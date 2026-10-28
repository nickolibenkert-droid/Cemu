package info.cemu.cemu.common.android.usb

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import androidx.core.content.ContextCompat
import info.cemu.cemu.nativeinterface.NativeLogging
import info.cemu.cemu.nativeinterface.NativeWiimote
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Mayflash DolphinBar in mode 4. It shows up as a Nintendo Wii Remote (057e:0306) with one
 * HID interface per Wii Remote slot. This object finds the bar, asks for USB permission, opens it
 * and hands the connection to the native Wii Remote code.
 */
object DolphinBar {
    private const val VENDOR_ID = 0x057e
    private const val PRODUCT_ID = 0x0306

    enum class State {
        NOT_FOUND,
        NO_PERMISSION,
        OPEN_FAILED,
        OPEN,
    }

    private val _state = MutableStateFlow(State.NOT_FOUND)
    val state = _state.asStateFlow()

    private lateinit var appContext: Context
    private var usbManager: UsbManager? = null

    private var device: UsbDevice? = null
    private var connection: UsbDeviceConnection? = null
    private var claimedInterfaces: List<UsbInterface> = emptyList()
    private var permissionRequestedFor: String? = null

    private val permissionAction: String
        get() = "${appContext.packageName}.DOLPHINBAR_PERMISSION"

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == UsbManager.ACTION_USB_DEVICE_DETACHED) {
                permissionRequestedFor = null
            }
            refresh()
        }
    }

    fun init(context: Context) {
        appContext = context.applicationContext
        usbManager = appContext.getSystemService(UsbManager::class.java) ?: return

        val filter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            addAction(permissionAction)
        }
        ContextCompat.registerReceiver(
            appContext,
            receiver,
            filter,
            ContextCompat.RECEIVER_NOT_EXPORTED
        )

        refresh()
    }

    /** Asks for USB permission again, for when it was denied earlier */
    @Synchronized
    fun retry() {
        permissionRequestedFor = null
        refresh()
    }

    @Synchronized
    fun refresh() {
        val usbManager = usbManager ?: return
        val bar = usbManager.deviceList.values.firstOrNull {
            it.vendorId == VENDOR_ID && it.productId == PRODUCT_ID
        }

        if (bar == null) {
            close()
            _state.value = State.NOT_FOUND
            return
        }

        if (connection != null && device?.deviceName == bar.deviceName) {
            return
        }

        close()

        if (!usbManager.hasPermission(bar)) {
            _state.value = State.NO_PERMISSION
            requestPermission(usbManager, bar)
            return
        }

        open(usbManager, bar)
    }

    private fun requestPermission(usbManager: UsbManager, bar: UsbDevice) {
        // ask once per plug-in, refresh() runs again when the answer arrives
        if (permissionRequestedFor == bar.deviceName) {
            return
        }
        permissionRequestedFor = bar.deviceName

        val intent = Intent(permissionAction).setPackage(appContext.packageName)
        val pendingIntent = PendingIntent.getBroadcast(
            appContext,
            0,
            intent,
            PendingIntent.FLAG_IMMUTABLE
        )
        NativeLogging.log("DolphinBar: requesting USB permission")
        usbManager.requestPermission(bar, pendingIntent)
    }

    private fun open(usbManager: UsbManager, bar: UsbDevice) {
        _state.value = State.OPEN_FAILED
        val newConnection = usbManager.openDevice(bar)
        if (newConnection == null) {
            NativeLogging.log("DolphinBar: failed to open USB device")
            return
        }

        val interfaces = mutableListOf<UsbInterface>()
        val interfaceNumbers = mutableListOf<Int>()
        val inEndpoints = mutableListOf<Int>()

        // one interface per Wii Remote slot, each with one interrupt IN endpoint.
        // output reports go through the control endpoint
        for (i in 0 until minOf(bar.interfaceCount, NativeWiimote.MAX_WIIMOTES)) {
            val usbInterface = bar.getInterface(i)
            val inEndpoint = (0 until usbInterface.endpointCount)
                .map { usbInterface.getEndpoint(it) }
                .firstOrNull {
                    it.direction == UsbConstants.USB_DIR_IN && it.type == UsbConstants.USB_ENDPOINT_XFER_INT
                } ?: continue

            if (!newConnection.claimInterface(usbInterface, true)) {
                NativeLogging.log("DolphinBar: failed to claim interface ${usbInterface.id}")
                continue
            }

            interfaces += usbInterface
            interfaceNumbers += usbInterface.id
            inEndpoints += inEndpoint.address
        }

        if (interfaces.isEmpty()) {
            // Android sometimes reports no interfaces until the bar is plugged in again
            NativeLogging.log("DolphinBar: no usable interfaces (${bar.interfaceCount}), try reconnecting it")
            newConnection.close()
            return
        }

        device = bar
        connection = newConnection
        claimedInterfaces = interfaces
        NativeWiimote.setAdapter(
            newConnection.fileDescriptor,
            interfaceNumbers.toIntArray(),
            inEndpoints.toIntArray()
        )
        NativeLogging.log("DolphinBar: opened ${bar.deviceName} with ${interfaces.size} slots")
        _state.value = State.OPEN
    }

    private fun close() {
        val connection = connection ?: return

        // native readers use the file descriptor, stop them before it is closed
        NativeWiimote.clearAdapter()
        claimedInterfaces.forEach { connection.releaseInterface(it) }
        connection.close()

        this.connection = null
        device = null
        claimedInterfaces = emptyList()
    }
}
