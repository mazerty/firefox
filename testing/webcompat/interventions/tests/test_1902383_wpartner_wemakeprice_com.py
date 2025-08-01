import asyncio

import pytest

URL = "https://wpartner.wemakeprice.com/login"

FAILURE_TEXT = "크롬을"


@pytest.mark.asyncio
@pytest.mark.with_interventions
async def test_enabled(client):
    await client.navigate(URL)
    await asyncio.sleep(3)
    assert not client.find_text(FAILURE_TEXT, is_displayed=True)


@pytest.mark.asyncio
@pytest.mark.without_interventions
async def test_disabled(client):
    await client.navigate(URL, wait="none")
    assert client.await_text(FAILURE_TEXT, is_displayed=True)
