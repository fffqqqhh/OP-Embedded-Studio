---
title: 静态部署 Web 应用
description: 将 OpenPencil 构建并部署为不依赖业务后端的静态 Web 应用。
---

# 静态部署 Web 应用

这种部署方式用于将 OpenPencil 发布给其他人使用，不引入账号系统或中心文档服务。生产构建产物是一个静态单页应用，托管服务器只负责分发文件，不需要实现 OpenPencil 业务 API。

## 架构

```text
用户浏览器
  ├─ HTTPS ──> 自有静态服务器或 CDN
  │              └─ HTML、JavaScript、CSS、WASM、字体、固件和 PWA 资源
  ├─ IndexedDB ──> 本地文档、恢复数据、偏好设置和协作房间缓存
  ├─ WebRTC ──> 其他协作参与者
  └─ HTTPS ──> 用户按需配置的 AI、S3、矢量化和图库服务
```

在这种模式下，服务器不会接收或持久化设计文档。清除浏览器站点数据可能删除本地保存的数据，因此应提醒用户定期导出重要文档。协作房间链接和外部服务凭证也由客户端负责管理。

## 构建约定

在仓库根目录执行以下命令生成生产构建：

```sh
bun install --frozen-lockfile
bun run build:packages
bunx vite build
```

必须发布完整的 `dist/` 目录，不要重命名文件，也不要只选择其中部分文件。构建产物包含带内容哈希的 JavaScript 和 CSS、CanvasKit WASM、PWA 文件、内置字体、嵌入式显示屏配置和固件资源。

### 部署在域名根路径

如果应用部署在源站根路径，例如 `https://studio.example.com/`，不需要设置额外的构建变量。

### 部署在子路径

如果应用部署在某个子路径，构建时需要设置 `VITE_APP_BASE_URL`：

```sh
VITE_APP_BASE_URL=/embedded-studio/ bunx vite build
```

该值必须以 `/` 开头和结尾。构建路径必须与实际部署路径一致，因为该基础路径会影响 Vue Router 历史路由、静态资源 URL、Web App Manifest、Service Worker 作用域和离线导航回退。

## 静态服务器协议

服务器工程师只需满足以下 HTTP 约定。

### HTTPS

生产环境必须使用 HTTPS，并将 HTTP 永久重定向到 HTTPS。PWA 安装以及文件、剪贴板、USB 和凭证存储等浏览器功能都依赖安全上下文。

### HTTP 方法

服务器需要支持 `GET` 和 `HEAD`。静态应用不需要服务端提供 `POST`、`PUT`、`PATCH`、`DELETE`、WebSocket 或 Server-Sent Events 接口。

### SPA 路由回退

当浏览器导航到一个不存在的页面路径时，应返回 `index.html`。但这一回退不能用于缺失的静态资源：不存在的 `.js`、`.css`、`.wasm`、图片、字体、固件、Manifest 或 JSON 文件必须返回 `404`，不能返回 HTML。

这一区分可以避免浏览器把回退页面错误缓存为损坏的 WASM 或 JavaScript 文件。

### MIME 类型

服务器至少需要正确返回以下类型：

| 扩展名 | `Content-Type` |
| --- | --- |
| `.html` | `text/html; charset=utf-8` |
| `.js` | `text/javascript; charset=utf-8` |
| `.css` | `text/css; charset=utf-8` |
| `.wasm` | `application/wasm` |
| `.json`、`.webmanifest` | `application/json` 或 `application/manifest+json` |
| `.ttf` | `font/ttf` |
| `.png` | `image/png` |
| `.svg` | `image/svg+xml` |
| `.bin` | `application/octet-stream` |

`/canvaskit.wasm` 和 `/canvaskit-webgpu/canvaskit.wasm` 必须返回 `application/wasm`；部署在子路径时，需要在这两个地址前加上相应基础路径。

### 缓存策略

入口和控制文件应采用重新验证策略，只有文件名包含内容哈希的资源才能设置长期不可变缓存：

| 资源 | 建议的 `Cache-Control` |
| --- | --- |
| `index.html` | `no-cache` |
| `manifest.webmanifest` | `no-cache` |
| Service Worker 脚本 | `no-cache` |
| `assets/` 中带内容哈希的文件 | `public, max-age=31536000, immutable` |
| 不带哈希的 WASM、固件、字体和目录文件 | `public, max-age=0, must-revalidate` |

应以原子方式发布整个 `dist/` 目录。新版本 `index.html` 与旧版本资源混用，或者在边缘缓存更新前删除旧资源，都可能导致正在使用的会话加载失败。如果托管平台无法原子切换版本，应短期保留上一版本资源。

### 压缩和范围请求

为 HTML、JavaScript、CSS、JSON、SVG、WASM 和字体启用 Brotli 或 gzip。不要再次动态压缩已经压缩过的图片。建议为较大的静态二进制文件支持字节范围请求，但这不是应用运行的硬性要求。

### 安全响应头

建议采用以下基础安全响应头：

```http
X-Content-Type-Options: nosniff
Referrer-Policy: strict-origin-when-cross-origin
Permissions-Policy: camera=(), microphone=(), geolocation=()
```

在没有完整测试所有 AI、图片、字体、对象存储、信令和 TURN 服务之前，不要启用 `Cross-Origin-Embedder-Policy: require-corp`，否则可能阻止现有跨域资源加载。

建议配置 Content Security Policy，但具体的 `connect-src`、`img-src` 等白名单取决于部署时允许使用哪些外部服务。正式强制执行前，应先使用报告模式进行验证。

不要在 `VITE_*` 构建变量中写入 API Key、S3 密钥、TURN 密钥或其他私密配置。Vite 会将这些变量编译进任何人都能下载的浏览器资源中。

## Nginx 配置示例

以下示例将应用部署在独立域名根路径。请替换域名、证书路径和发布目录。

```nginx
map $uri $openpencil_cache_control {
    default                                         "public, max-age=0, must-revalidate";
    ~^/assets/.*\.[A-Za-z0-9_-]{8,}\.[^.]+$        "public, max-age=31536000, immutable";
    ~^/(index\.html|manifest\.webmanifest|sw\.js)$ "no-cache";
}

server {
    listen 80;
    server_name studio.example.com;
    return 308 https://$host$request_uri;
}

server {
    listen 443 ssl http2;
    server_name studio.example.com;

    ssl_certificate /etc/letsencrypt/live/studio.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/studio.example.com/privkey.pem;

    root /srv/openpencil/current;
    index index.html;

    add_header X-Content-Type-Options nosniff always;
    add_header Referrer-Policy strict-origin-when-cross-origin always;
    add_header Permissions-Policy "camera=(), microphone=(), geolocation=()" always;
    add_header Cache-Control $openpencil_cache_control always;

    gzip on;
    gzip_types text/css application/javascript application/json application/wasm image/svg+xml;

    location = /index.html {
        try_files $uri =404;
    }

    location ~* ^/assets/.*\.[A-Za-z0-9_-]{8,}\.(js|css|png|svg|woff2?)$ {
        try_files $uri =404;
    }

    location ~* \.(js|css|wasm|json|webmanifest|ttf|woff2?|png|jpe?g|svg|ico|bin)$ {
        try_files $uri =404;
    }

    location / {
        try_files $uri $uri/ /index.html;
    }
}
```

如果部署在子路径，需要使用 Nginx `alias`，或者正确配置 `location /embedded-studio/`，并将回退地址改为 `/embedded-studio/index.html`。构建时必须使用相同的 `VITE_APP_BASE_URL`。

## 容器构建示例

可以通过以下多阶段镜像构建并提供静态资源：

```dockerfile
FROM oven/bun:1.3.10 AS build
WORKDIR /app
COPY . .
RUN bun install --frozen-lockfile
RUN bun run build:packages
RUN bunx vite build

FROM nginx:1.29-alpine
COPY --from=build /app/dist /usr/share/nginx/html
COPY deploy/nginx.conf /etc/nginx/conf.d/default.conf
```

仓库当前没有提供 `deploy/nginx.conf`。应根据上述约定在部署基础设施中创建该文件，因为实际域名和证书配置属于部署环境。

## 外部网络依赖

没有业务后端时，应用仍可完成本地设计工作。部分可选功能会由浏览器直接访问外部服务：

| 功能 | 目标服务 | 对部署环境的要求 |
| --- | --- | --- |
| 实时协作 | Trystero MQTT 信令、STUN/TURN，随后连接其他参与者 | 企业防火墙需要允许 WebRTC 和已配置的信令、ICE 服务。 |
| AI 对话 | 用户选择的模型服务地址 | 服务商必须允许浏览器跨域请求，凭证由用户保存在本地。 |
| S3 存储 | 用户配置的对象存储地址 | Bucket CORS 必须允许当前部署源站。 |
| 图片矢量化 | Recraft 或 fal.ai | 浏览器必须能访问服务 API 和允许的 SVG 下载域名。 |
| 图库 | Pexels 或 Unsplash | 浏览器必须能访问服务 API 和图片域名。 |

如果目标网络无法连接这些服务，对应的可选功能将不可用。单纯部署静态站点不会代理或替代这些外部服务。

## 发布与回滚

1. 使用固定的提交和 lockfile 构建。
2. 执行适合当前发布范围的项目质量检查。
3. 将完整的 `dist/` 上传到带版本号的发布目录。
4. 原子切换当前生效版本。
5. 按照下方清单验收。
6. 需要回滚时，将生效路径切回上一份完整的 `dist/`。

PWA Service Worker 会自动更新，因此新版本可能要到下一次 Service Worker 更新周期才对用户生效。发布期间应保持资源兼容；如需立即验证新版本，可以要求测试用户重新加载页面。

## 验收清单

从服务器网络外执行以下检查：

```sh
curl -I https://studio.example.com/
curl -I https://studio.example.com/canvaskit.wasm
curl -I https://studio.example.com/canvaskit-webgpu/canvaskit.wasm
curl -I https://studio.example.com/a-route-that-does-not-exist
curl -I https://studio.example.com/assets/does-not-exist.js
```

预期结果：

- HTTP 会重定向到 HTTPS。
- `/` 返回 `200` 和 HTML。
- 两个 CanvasKit 地址都返回 `200`，并包含 `Content-Type: application/wasm`。
- 不存在的页面路由返回应用的 `index.html`。
- 不存在的静态资源返回 `404`，不能返回 `index.html`。
- 浏览器开发者工具中没有生产资源加载失败。
- 可以创建、重新打开、导出和从本地恢复文档。
- 支持的浏览器可以成功安装 PWA。
- 实时协作和每个计划启用的外部服务需要分别测试。

## 服务器工程师不需要实现的内容

这种部署方式不需要 OpenPencil REST API、数据库、身份认证服务、文档 WebSocket 服务、服务端渲染器或文件上传接口。如果后续增加其中任何一项，产品就会转变成私有云文档平台，需要另行设计业务协议并修改客户端。
