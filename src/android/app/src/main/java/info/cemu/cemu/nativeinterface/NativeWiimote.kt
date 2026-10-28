package info.cemu.cemu.nativeinterface

object NativeWiimote {
    const val NO_WIIMOTE: Int = -1
    const val MAX_WIIMOTES: Int = 4

    @JvmStatic
    external fun setAdapter(fd: Int, interfaceNumbers: IntArray, inEndpoints: IntArray)

    @JvmStatic
    external fun clearAdapter()

    @JvmStatic
    external fun getConnectedWiimotes(): BooleanArray

    @JvmStatic
    external fun getRealWiimote(index: Int): Int

    @JvmStatic
    external fun setRealWiimote(index: Int, wiimoteIndex: Int)
}
