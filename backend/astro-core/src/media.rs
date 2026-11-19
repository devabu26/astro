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

    pub async fn video(
        &self,
        source: MediaSource,
    ) -> Result<Vec<u8>, String> {
        let request = MediaRequestParameters {
            source,
            format: MediaFormat::File,
        };

        self.client
            .media()
            .get_media_content(&request, true)
            .await
            .map_err(|e| format!("Video download failed: {e}"))
    }

    pub async fn image(
        &self,
        source: MediaSource,
    ) -> Result<Vec<u8>, String> {
        let format = match &source {
            MediaSource::Encrypted(_) => MediaFormat::File,
            MediaSource::Plain(_) => {
                MediaFormat::Thumbnail(
                    MediaThumbnailSettings::new(1280u32.into(), 1280u32.into()),
                )
            }
        };

        let request = MediaRequestParameters {
            source,
            format,
        };

        self.client
            .media()
            .get_media_content(&request, true)
            .await
            .map_err(|e| format!("Image download failed: {e}"))
    }
}
