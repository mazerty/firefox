/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

package org.mozilla.geckoview.test

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.filters.MediumTest
import org.hamcrest.Matchers.equalTo
import org.hamcrest.Matchers.not
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.mozilla.gecko.util.ThreadUtils
import org.mozilla.geckoview.AllowOrDeny
import org.mozilla.geckoview.GeckoResult
import org.mozilla.geckoview.GeckoRuntime.ServiceWorkerDelegate
import org.mozilla.geckoview.GeckoSession
import org.mozilla.geckoview.GeckoSession.ContentDelegate
import org.mozilla.geckoview.GeckoSession.NavigationDelegate
import org.mozilla.geckoview.GeckoSession.NavigationDelegate.LoadRequest
import org.mozilla.geckoview.GeckoSession.PermissionDelegate
import org.mozilla.geckoview.WebNotification
import org.mozilla.geckoview.WebNotificationDelegate
import org.mozilla.geckoview.test.rule.GeckoSessionTestRule
import org.mozilla.geckoview.test.rule.GeckoSessionTestRule.AssertCalled
import org.mozilla.geckoview.test.rule.GeckoSessionTestRule.NullDelegate
import org.mozilla.geckoview.test.util.UiThreadUtils

@RunWith(AndroidJUnit4::class)
@MediumTest
class OpenWindowTest : BaseSessionTest() {

    @Before
    fun setup() {
        sessionRule.setPrefsUntilTestEnd(mapOf("dom.webnotifications.requireuserinteraction" to false))

        // Grant "desktop notification" permission
        mainSession.delegateUntilTestEnd(object : PermissionDelegate {
            override fun onContentPermissionRequest(session: GeckoSession, perm: PermissionDelegate.ContentPermission): GeckoResult<Int>? {
                assertThat("Should grant DESKTOP_NOTIFICATIONS permission", perm.permission, equalTo(PermissionDelegate.PERMISSION_DESKTOP_NOTIFICATION))
                return GeckoResult.fromValue(PermissionDelegate.ContentPermission.VALUE_ALLOW)
            }
        })
    }

    private fun openPageClickNotification(teardownAlertsService: Boolean = false, urlParam: String = "") {
        mainSession.loadUri(OPEN_WINDOW_PATH + urlParam)
        sessionRule.waitForPageStop()
        val result = mainSession.waitForJS("Notification.requestPermission()")
        assertThat(
            "Permission should be granted",
            result as String,
            equalTo("granted"),
        )

        val notificationResult = GeckoResult<Void>()
        var notificationShown: WebNotification? = null

        sessionRule.delegateDuringNextWait(object : WebNotificationDelegate {
            @GeckoSessionTestRule.AssertCalled
            override fun onShowNotification(notification: WebNotification) {
                notificationShown = notification
                notificationResult.complete(null)
            }
        })
        mainSession.evaluateJS("showNotification()")
        sessionRule.waitForResult(notificationResult)

        if (teardownAlertsService) {
            mainSession.teardownAlertsService()
        }

        notificationShown!!.click()
    }

    @Test
    @NullDelegate(ServiceWorkerDelegate::class)
    fun openWindowNullDelegate() {
        sessionRule.delegateUntilTestEnd(object : ContentDelegate, NavigationDelegate {
            override fun onLocationChange(session: GeckoSession, url: String?, perms: MutableList<PermissionDelegate.ContentPermission>, hasUserGesture: Boolean) {
                // we should not open the target url
                assertThat("URL should notmatch", url, not(OPEN_WINDOW_TARGET_PATH))
            }
        })
        openPageClickNotification()
        UiThreadUtils.loopUntilIdle(sessionRule.env.defaultTimeoutMillis)
    }

    @Test
    fun openWindowNullResult() {
        sessionRule.delegateUntilTestEnd(object : ContentDelegate, NavigationDelegate {
            override fun onLocationChange(session: GeckoSession, url: String?, perms: MutableList<PermissionDelegate.ContentPermission>, hasUserGesture: Boolean) {
                // we should not open the target url
                assertThat("URL should notmatch", url, not(OPEN_WINDOW_TARGET_PATH))
            }
        })
        openPageClickNotification()
        sessionRule.waitUntilCalled(object : ServiceWorkerDelegate {
            @AssertCalled(count = 1)
            override fun onOpenWindow(url: String): GeckoResult<GeckoSession> {
                ThreadUtils.assertOnUiThread()
                return GeckoResult.fromValue(null)
            }
        })
    }

    @Test
    fun openWindowSameSession() {
        sessionRule.delegateUntilTestEnd(object : ServiceWorkerDelegate {
            @AssertCalled(count = 1)
            override fun onOpenWindow(url: String): GeckoResult<GeckoSession> {
                ThreadUtils.assertOnUiThread()
                return GeckoResult.fromValue(mainSession)
            }
        })
        openPageClickNotification()
        sessionRule.waitUntilCalled(object : ContentDelegate, NavigationDelegate {
            @AssertCalled(count = 1, order = [1])
            override fun onLocationChange(session: GeckoSession, url: String?, perms: MutableList<PermissionDelegate.ContentPermission>, hasUserGesture: Boolean) {
                assertThat("Should be on the main session", session, equalTo(mainSession))
                assertThat("URL should match", url, equalTo(OPEN_WINDOW_TARGET_PATH))
            }

            @AssertCalled(count = 1, order = [2])
            override fun onTitleChange(session: GeckoSession, title: String?) {
                assertThat("Should be on the main session", session, equalTo(mainSession))
                assertThat("Title should be correct", title, equalTo("Open Window test target"))
            }
        })
    }

    @Test
    fun openWindowNewSession() {
        var targetSession: GeckoSession? = null
        sessionRule.delegateUntilTestEnd(object : ServiceWorkerDelegate {
            @AssertCalled(count = 1)
            override fun onOpenWindow(url: String): GeckoResult<GeckoSession> {
                ThreadUtils.assertOnUiThread()
                targetSession = sessionRule.createOpenSession()
                return GeckoResult.fromValue(targetSession)
            }
        })
        openPageClickNotification()
        sessionRule.waitUntilCalled(object : ContentDelegate, NavigationDelegate {
            @AssertCalled(count = 1, order = [1])
            override fun onLocationChange(session: GeckoSession, url: String?, perms: MutableList<PermissionDelegate.ContentPermission>, hasUserGesture: Boolean) {
                assertThat("Should be on the target session", session, equalTo(targetSession))
                assertThat("URL should match", url, equalTo(OPEN_WINDOW_TARGET_PATH))
            }

            @AssertCalled(count = 1, order = [2])
            override fun onTitleChange(session: GeckoSession, title: String?) {
                assertThat("Should be on the target session", session, equalTo(targetSession))
                assertThat("Title should be correct", title, equalTo("Open Window test target"))
            }
        })
    }

    @Test
    fun openWindowNewClosedSession() {
        var targetSession: GeckoSession? = null
        sessionRule.delegateUntilTestEnd(object : ServiceWorkerDelegate {
            @AssertCalled(count = 1)
            override fun onOpenWindow(url: String): GeckoResult<GeckoSession> {
                ThreadUtils.assertOnUiThread()
                assertThat("URL should match", url, equalTo(OPEN_WINDOW_TARGET_PATH))
                targetSession = sessionRule.createClosedSession()
                return GeckoResult.fromValue(targetSession)
            }
        })
        openPageClickNotification()
        sessionRule.waitUntilCalled(object : ContentDelegate, NavigationDelegate {
            @AssertCalled(count = 2, order = [1, 2])
            override fun onLocationChange(session: GeckoSession, url: String?, perms: MutableList<PermissionDelegate.ContentPermission>, hasUserGesture: Boolean) {
                assertThat("Should be on the target session", session, equalTo(targetSession))
                assertThat("URL should match", url, equalTo(forEachCall("about:blank", OPEN_WINDOW_TARGET_PATH)))
            }

            @AssertCalled(count = 1, order = [3])
            override fun onLoadRequest(session: GeckoSession, request: LoadRequest): GeckoResult<AllowOrDeny>? {
                assertThat("Should be on the target session", session, equalTo(targetSession))
                assertThat("URL should match", request.uri, equalTo(OPEN_WINDOW_TARGET_PATH))
                return null
            }

            @AssertCalled(count = 1, order = [4])
            override fun onTitleChange(session: GeckoSession, title: String?) {
                assertThat("Should be on the target session", session, equalTo(targetSession))
                assertThat("Title should be correct", title, equalTo("Open Window test target"))
            }
        })
    }

    @Test
    fun openWindowAfterAlertServiceTeardown() {
        sessionRule.delegateUntilTestEnd(object : ServiceWorkerDelegate {
            @AssertCalled(count = 1)
            override fun onOpenWindow(url: String): GeckoResult<GeckoSession> {
                ThreadUtils.assertOnUiThread()
                return GeckoResult.fromValue(mainSession)
            }
        })
        openPageClickNotification(teardownAlertsService = true)
        sessionRule.waitUntilCalled(object : ContentDelegate, NavigationDelegate {
            @AssertCalled(count = 1)
            override fun onLocationChange(session: GeckoSession, url: String?, perms: MutableList<PermissionDelegate.ContentPermission>, hasUserGesture: Boolean) {
                assertThat("Should be on the main session", session, equalTo(mainSession))
                assertThat("URL should match", url, equalTo(OPEN_WINDOW_TARGET_PATH))
            }
        })
    }

    @Test
    fun openWindowWithActionAfterAlertServiceTeardown() {
        sessionRule.delegateUntilTestEnd(object : ServiceWorkerDelegate {
            @AssertCalled(count = 1)
            override fun onOpenWindow(url: String): GeckoResult<GeckoSession> {
                ThreadUtils.assertOnUiThread()
                return GeckoResult.fromValue(mainSession)
            }
        })
        openPageClickNotification(teardownAlertsService = true, urlParam = "?action=foo")
        sessionRule.waitUntilCalled(object : ContentDelegate, NavigationDelegate {
            @AssertCalled(count = 1)
            override fun onLocationChange(session: GeckoSession, url: String?, perms: MutableList<PermissionDelegate.ContentPermission>, hasUserGesture: Boolean) {
                assertThat("Should be on the main session", session, equalTo(mainSession))
                assertThat("URL should match", url, equalTo("$OPEN_WINDOW_TARGET_PATH?action=foo"))
            }
        })
    }
}
