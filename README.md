# 墨卡 / English Flash Cards

面向 4.2 英寸三色电子墨水屏的英语单词闪卡服务。FastAPI 同时提供管理网页和 JSON API；SQLite 保存词卡资料，PNG 文件保存在本地。

## 启动

```bash
cp .env.example .env
# 在 .env 填入 OPENAI_API_KEY
uv sync
uv run --env-file .env uvicorn main:app --reload
```

访问 <http://127.0.0.1:8000>。首次运行会创建 `data/flashcards.db`；已有数据库会自动迁移新增的单词本和图片状态字段。

## 工作方式

1. 新建单词本，并将一个单词加入任意多个单词本；也可在词卡上直接调整归属。
2. 输入单词后点击“自动补全”：服务从 Dictionary API 获取 IPA 与简明释义，并用本地规则估算音节。结果是可编辑的建议，不会覆盖手工内容。超过 52 个字符的释义不会显示在闪卡上，也不会传给图片生成模型。
3. 单张“强制重新生成”必定调用 OpenAI；“后台批量生成”会跳过已存在的 `data/cards/<word>.png`，并在网页显示进度与失败状态。
4. 点击“发送至 EPD”：服务会向 `EPD_UPLOAD_URL` 以 `multipart/form-data` 上传字段 `file`（PNG）、`word`、`width`、`height` 与 `colours`。若网关需要认证，填入 `EPD_API_TOKEN`，会作为 Bearer token 发送。

## API

| 方法 | 地址 | 作用 |
| --- | --- | --- |
| GET /api/cards | 列出单词卡 |
| POST /api/cards | 新建单词卡 |
| PUT /api/cards/{id} | 修改词卡文字资料 |
| PUT /api/cards/{id}/books | 替换该单词所属的单词本 |
| DELETE /api/cards/{id} | 删除词卡与本地图片 |
| GET/POST/DELETE /api/books | 管理单词本 |
| POST /api/cards/enrich | 自动建议 IPA、音节、记忆提示 |
| POST /api/cards/{id}/generate | 用 OpenAI 生成插图并合成闪卡 |
| POST /api/images/generate-batch | 后台生成当前范围的缺失图片 |
| GET /api/images/progress | 查询批量生成进度 |
| GET /api/cards/{id}/image | 获取 400×300 PNG |
| POST /api/cards/{id}/review | 记录一次复习 |
| POST /api/cards/{id}/epd | 上传 PNG 到 EPD 网关 |

`POST /api/cards` 的请求体示例：

```json
{"word":"apple","ipa":"/ˈæp.əl/","syllables":"ap-ple","hint":"a red apple"}
```
