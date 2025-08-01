import pytest

URL = "https://www.schoolnutritionandfitness.com/webmenus2/#/view?id=6331c49ce96f1e9c468b45be&siteCode=1641"
POPUP_CLOSE_CSS = ".modal-dialog button"
ELEM_CSS = "react-app td > div"
HEIGHT_CUTOFF = 10


def get_elem_height(client):
    elem = client.await_css(ELEM_CSS, is_displayed=True)
    assert elem
    return client.execute_script(
        """
        return arguments[0].getBoundingClientRect().height;
    """,
        elem,
    )


@pytest.mark.asyncio
@pytest.mark.with_interventions
async def test_enabled(client):
    await client.navigate(URL)
    client.soft_click(client.await_css(POPUP_CLOSE_CSS, is_displayed=True))
    assert get_elem_height(client) > HEIGHT_CUTOFF


@pytest.mark.asyncio
@pytest.mark.without_interventions
async def test_disabled(client):
    await client.ensure_InstallTrigger_undefined()
    await client.navigate(URL)
    assert get_elem_height(client) < HEIGHT_CUTOFF
