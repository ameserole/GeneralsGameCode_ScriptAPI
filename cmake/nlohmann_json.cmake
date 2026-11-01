# Fetch nlohmann/json for the script API protocol (header-only).

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    EXCLUDE_FROM_ALL
    URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)

FetchContent_MakeAvailable(nlohmann_json)
