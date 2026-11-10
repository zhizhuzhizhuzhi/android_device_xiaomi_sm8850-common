package com.xiaomi.settings.touch

import android.os.IBinder
import android.os.ServiceManager
import vendor.xiaomi.hw.touchfeature.ITouchFeature

object TouchFeatureWrapper {
    @Volatile private var touchFeature: ITouchFeature? = null
    private val deathRecipient = IBinder.DeathRecipient { touchFeature = null }

    @Synchronized
    private fun getTouchFeature(): ITouchFeature {
        touchFeature?.let { return it }
        val name = "${ITouchFeature.DESCRIPTOR}/default"
        val binder = ServiceManager.checkService(name)
            ?: error("Touch HAL is not registered")
        val service = checkNotNull(ITouchFeature.Stub.asInterface(binder))
        binder.linkToDeath(deathRecipient, 0)
        touchFeature = service
        return service
    }

    fun getReportRate(): Boolean {
        val service = getTouchFeature()
        val active = service.getModeCurValue(0, 202)
        check(active in 0..1) {
            "Touch report-rate state is unavailable: $active"
        }
        return active == 1
    }

    fun setModeValue(type: Int, mode: Int, value: Int) {
        val result = getTouchFeature().setModeValue(type, mode, value)
        check(result == 0) { "Touch HAL mode $mode failed: $result" }
    }
}
