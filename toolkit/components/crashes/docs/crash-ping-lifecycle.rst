====================
Crash Ping Lifecycle
====================

Crash pings and derived data go through a number of separate programs and
services. To get a better idea of how these components interact, a breakdown of
the lifecycle is presented here.

This description applies to Glean crash ping data.


Origin
======
When a crash occurs, Glean metrics are populated and a Glean crash ping is sent with the data. This
is ingested and made available in BigQuery through the usual Glean infrastructure.

Ping Definitions
----------------
* `Desktop crash ping <https://dictionary.telemetry.mozilla.org/apps/firefox_desktop/pings/crash>`_

  * `Desktop metrics definition
    <https://searchfox.org/mozilla-central/source/toolkit/components/crashes/metrics.yaml>`_
  * `Desktop ping definition
    <https://searchfox.org/mozilla-central/source/toolkit/components/crashes/pings.yaml>`_

* `Fenix crash ping <https://dictionary.telemetry.mozilla.org/apps/fenix/pings/crash>`_

  * `Fenix metrics definition
    <https://searchfox.org/mozilla-central/source/mobile/android/android-components/components/lib/crash/metrics.yaml>`_
  * `Fenix ping definition
    <https://searchfox.org/mozilla-central/source/mobile/android/android-components/components/lib/crash/pings.yaml>`_

BigQuery Tables
---------------
* Desktop view: ``firefox_desktop.crash``.
* Crashreporter client view: ``firefox_crashreporter.crash``. This uses the same metrics/ping definitions
  as desktop.
* Fenix view: ``fenix.crash``. This ping has a few different metrics, but is overall very similar to
  the desktop ping. As a result, it's a little verbose to combine fenix and desktop pings in a
  query, however most metrics exist in both with the same name.
* Combined view (**HIGHLY RECOMMENDED, IMPLEMENTS THE NOTES BELOW**): ``telemetry.firefox_crashes``.
  This combines the pings from desktop, the crashreporter client, fenix, focus, and klar. It merges
  the metrics structs of desktop and android in an obvious way (essentially similar to an outer
  join). It also deduplicates based on minidump hash and adds ``crash_app_channel``,
  ``crash_app_display_version``, and ``crash_app_build`` columns.

**NOTES**

* When querying the source data, you should always prefer the ``crash.app_channel``,
  ``crash.app_display_version``, and ``crash.app_build`` metrics rather than the similarly named
  fields of the Glean ``client_info`` struct. These values correspond to the application information
  *at the time of the crash*, and moreover the crash reporter client can't fully populate the
  client_info. However, it is best to fall back to the ``client_info`` if these are null (see `bug
  1966213 <https://bugzilla.mozilla.org/show_bug.cgi?id=1966213>`__). Java exception pings on Fenix
  are known to be missing these metrics (see `bug 1966210
  <https://bugzilla.mozilla.org/show_bug.cgi?id=1966210>`__). This can be easily achieved with e.g.
  ``IFNULL(metrics.string.crash_app_channel, client_info.app_channel)``.

* We may receive duplicate pings corresponding to the same crash event. You should be careful to
  deduplicate based on minidump hash if doing anything pertaining to counting crash events.

  * You can deduplicate with a BigQuery ``QUALIFY`` clause like:

    .. code-block:: sql

      QUALIFY
        metrics.string.crash_minidump_sha256_hash = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
        OR metrics.string.crash_minidump_sha256_hash IS NULL
        OR ROW_NUMBER() OVER(PARTITION BY metrics.string.crash_minidump_sha256_hash ORDER BY submission_timestamp) = 1

    This will dedup any non-null and non-empty minidump hashes.


Source
------
All crash ping metrics are set in bulk at the same time, and typically come directly from `crash annotations <https://searchfox.org/mozilla-central/source/toolkit/crashreporter/CrashAnnotations.yaml>`_:

* `Desktop <https://searchfox.org/mozilla-central/rev/b598575345077063c55b618e43ccaa6249505d02/toolkit/components/crashes/CrashManager.in.sys.mjs#787>`__
* `Crashreporter client <https://searchfox.org/mozilla-central/rev/b598575345077063c55b618e43ccaa6249505d02/toolkit/crashreporter/client/app/src/net/ping/glean.rs#11>`__
* `Fenix <https://searchfox.org/mozilla-central/rev/b598575345077063c55b618e43ccaa6249505d02/mobile/android/android-components/components/lib/crash/src/main/java/mozilla/components/lib/crash/service/GleanCrashReporterService.kt#312>`__


Post-Processing
===============
The `crash-ping-ingest <https://github.com/mozilla/crash-ping-ingest>`_ repo is scheduled (using
taskcluster) to run daily ingestion. It will retrieve crash pings with submissions as recent as the
prior UTC day, ensuring that indexed results for the past week are available by default (in case of
outages/hiccups/etc). This runs at 2:00 UTC and takes 1-2 hours, so you can expect data to be
availalbe for the prior UTC day around 4:00 UTC. It also supplies a taskcluster action to manually
generate data for a given date, if necessary.

Data Availability
-----------------
Data was backfilled to 2024-09-01, so you can expect ping data to be available for any date after
then. All nightly and beta pings are processed, while release pings are randomly sampled with about
5000 pings per os/process-type combination.

BigQuery
--------
The ingested output (including symbolicated stacks and crash signatures) is loaded into BigQuery in
the ``moz-fx-data-shared-prod.crash_ping_ingest_external.ingest_output`` table. It is partitioned on
``submission_timestamp`` to match the Glean views/tables, and it can be joined on ``document_id``
(and optionally ``submission_timestamp``) with the fenix/desktop/combined views.

What if post-processing has a bug?
----------------------------------
If there's a problem with the post-processed output, the post-processing bug can be fixed and the
data can be re-generated by running the ingestion for the day(s) affected. The `upload script in
crash-ping-ingest <https://github.com/mozilla/crash-ping-ingest/blob/main/upload.py>`_ will
*replace* the data for the uploading date automatically. To run the ingestion, you must navigate to
the taskcluster **task group** for the commits with the fixes (this is easily found by going to the
taskcluster CI page for the commit on GitHub) and run the action task for "Process Pings (Manual)".
There you can choose which dates to run.

Once the data in BigQuery has been fixed, you must also clear the netlify ``ping-data`` blobs
corresponding to the affected dates. This can be done using the netlify-cli (though you need to
authenticate with netlify, of course).


Presentation
============
The `crash-pings <https://github.com/mozilla/crash-pings>`_ repository contains the code for the
website hosted on netlify: https://crash-pings.mozilla.org. See the README for details about how it
is built and what technologies it uses. It queries BigQuery and caches results, condensing data for
efficient loading in the browser.


Adding data to crash pings
==========================
#. Add crash annotations to the `definition file
   <https://searchfox.org/mozilla-central/source/toolkit/crashreporter/CrashAnnotations.yaml>`_ and
   populate the annotations with the generated APIs.
#. Define corresponding glean metrics to the files listed in `Ping Definitions`_.
#. Update the code that populates the metrics listed in `Source`_.
