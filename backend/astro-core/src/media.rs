use matrix_sdk::{
    media::{
        MediaFormat,
        MediaRequestParameters,
        MediaThumbnailSettings,
    },
    ruma::events::room::MediaSource,
    Client,
};

pub struct MediaManager<'a> {
    client: &'a Client,
}

impl<'a> MediaManager<'a> {
    pub fn new(client: &'a Client) -> Self {
        Self { client }
    }

    pub async fn thumbnail(
        &self,
        source: MediaSource,
        width: u32,
        height: u32,
    ) -> Result<Vec<u8>, String> {
        let request = MediaRequestParameters {
            source,
            format: MediaFormat::Thumbnail(
                MediaThumbnailSettings::new(width.into(), height.into()),
            ),
        };

        self.client
            .media()
            .get_media_content(&request, true)
            .await
            .map_err(|e| format!("Thumbnail download failed: {e}"))
    }
}
